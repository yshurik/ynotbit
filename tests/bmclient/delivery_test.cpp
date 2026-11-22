// bmc::Delivery without Qt: the outbox state machine, the spool and the receipts the relay writes,
// and the scan that reads the node's store. The fixtures are the same as tests/delivery_tests.cpp,
// which pins the same behaviour through the Qt veneer.
#include "fake_node.h"
#include <atomic>
#include <bmc/cache.h>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/delivery.h>
#include <bmc/mailbox.h>
#include <bmc/pow.h>
#include <bmc/secret.h>
#include <bmc/test.h>
#include <bmc/updates.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <ntb-object-db.h>
#include <optional>
#include <thread>
#include <vector>
extern "C" {
#include <ntb-address.h>
}

namespace {
// One simulated instant for a whole test: the proof of work needs real time, the protocol does
// not need the clock to move. It is anchored on the real clock, because the mailbox's own expiry
// checks read bmc::now(), and so does the solver, which works out its target from the time that is
// left to the object. An instant that is minutes old would ask of the object a harder target than
// the one it was solved for, and about one object in a few hundred would fail the check of the
// relay. So each test takes a new instant as it begins (when its first side is made).
int64_t &instant() {
    static int64_t value = bmc::now();
    return value;
}
int64_t kNow() {
    return instant();
}

// One end of the conversation: a vault with one identity, its mailbox, the node's object store and
// the delivery engine over them.
struct Side {
    std::string root, identity, vaultPath, mailPath, key;
    bmc::Vault vault;
    bmc::Mailbox mailbox;
    std::unique_ptr<bmc::Cache> cache;
    std::unique_ptr<bmc::Delivery> delivery;

    Side(const std::string &name, const char *label) : root(bmc::test::tempDir() + "/" + name) {
        instant() = bmc::now();
        std::filesystem::create_directories(bmc::path(root));
        vaultPath = root + "/v.bmvault";
        mailPath = root + "/m.bmmail";
        vault.create(vaultPath, "slice3-delivery");
        key = vault.addMailboxKey();
        mailbox.create(mailPath, key, vault.mailboxKey(key));
        identity = vault.addIdentity(label);
        cache = std::make_unique<bmc::Cache>(root);
        delivery = std::make_unique<bmc::Delivery>(root);
        mailbox.bindCache(cache->id());
    }
};

// A valid address of version 4 in stream 1 for each number: nobody holds its keys, which is all a
// subscription to it needs.
std::string anAddress(uint32_t n) {
    ntb_address a{};
    a.version = 4;
    a.stream = 1;
    a.ripe[0] = 0x11; // not a null byte, which an address leaves out
    for (int i = 0; i < 4; ++i)
        a.ripe[1 + i] = uint8_t(n >> (8 * i));
    a.ripe[19] = 0x7f;
    char text[NTB_ADDRESS_MAX_LENGTH + 1] = {};
    ntb_address_encode(&a, text);
    return text;
}

// Holds the searches of the proof of work where they are, until it goes out of scope, however that
// happens.
struct PowHeld {
    PowHeld() {
        bmc::ProofOfWork::holdForTesting(true);
    }
    ~PowHeld() {
        bmc::ProofOfWork::holdForTesting(false);
    }
};

// Turns both ends until `done`; the proof of work runs on its own threads, so the wall clock is the
// only deadline. A letter's way is a few searches in a row, each as long as it happens to be (the
// public key of a person lives 28 days, and that one is the longest: on the Windows runner the
// way took 5 to 39 s, and one run in several took over 60), so the deadline is far past any mean.
bool pump(Side &a, Side &b, const std::function<bool()> &done, int ms = 240000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        // A reads the receipts the relay wrote and spools what is ready.
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        // B reads what arrived, then answers.
        b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
        b.delivery->tick(b.mailbox, b.vault, true, kNow());
        // A reads B's objects from the round before, so A's own receipt is always processed
        // before B's acknowledgment reaches it.
        a.delivery->scan(*a.cache, a.mailbox, a.vault, kNow());
        fake::deliver(b.root, a.root, kNow());
        if (!done())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}
} // namespace

TEST(an_unknown_recipient_asks_for_its_key) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Hello Bob", "Body");
    a.mailbox.queueDraft(id, "direct", kNow() + 3600);

    a.delivery->tick(a.mailbox, a.vault, false, kNow());
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("awaiting_pubkey"));
    const auto jobs = a.mailbox.jobs();
    CHECK_EQ(jobs.size(), size_t(1));
    CHECK_EQ(jobs[0].kind, std::string("getpubkey"));
    CHECK(bmc::Wire::requestsIdentity(jobs[0].payload, b.vault.identities()[0]));
    a.delivery->stop();
}

TEST(a_broadcast_is_published_without_a_recipient_acknowledgment) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Announcement", "Everyone");
    a.mailbox.queueDraft(id, "broadcast", kNow() + 3600);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (a.mailbox.outgoing(id).state != "published" &&
           std::chrono::steady_clock::now() < deadline) {
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // The relay took the object: no recipient receipt is expected, so the delivery is finished.
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("published"));
    CHECK(!a.mailbox.outgoing(id).objectHash.empty());
    CHECK_EQ(a.mailbox.message(id).folder, std::string("Sent"));
}

// A broadcast of `bytes` bytes that lives `seconds`: the work it needs is about
// 1000 x (bytes + 1000) x (1 + seconds / 65536) trials.
static std::string queueBroadcast(Side &a, Side &b, const char *subject, size_t bytes,
                                  int64_t seconds) {
    const auto id =
        a.mailbox.saveDraft({}, a.identity, b.identity, subject, std::string(bytes, 'x'));
    a.mailbox.queueDraft(id, "broadcast", kNow() + seconds);
    return id;
}

TEST(a_small_object_is_mined_while_a_big_one_waits) {
    Side a("alice", "Alice"), b("bob", "Bob");
    // 200 KB for 28 days is about 8e9 trials: minutes of work for every core of any machine that
    // runs this, so the big one is still being mined when the test ends.
    const auto big = queueBroadcast(a, b, "Big", 200000, 28 * 86400);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    CHECK(a.delivery->working());
    // The small one (about 1.4e6 trials) comes after it and must not wait for it: a bot's short
    // reply to a chat is such an object, and a page of a block's transactions is the big one.
    const auto small = queueBroadcast(a, b, "Small", 100, 3600);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    // The job that was set aside says "queued", the one being mined says "mining".
    const auto jobState = [&](const std::string &owner) {
        for (const auto &j : a.mailbox.jobs())
            if (j.owner == owner)
                return j.state;
        return std::string("none");
    };
    CHECK_EQ(jobState(small), std::string("mining"));
    CHECK_EQ(jobState(big), std::string("queued"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (a.mailbox.outgoing(small).state != "published" &&
           std::chrono::steady_clock::now() < deadline) {
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_EQ(a.mailbox.outgoing(small).state, std::string("published"));
    // The big one is set aside, not given up: it is mined again as soon as nothing smaller is left.
    CHECK_EQ(a.mailbox.outgoing(big).state, std::string("calculating_message"));
    CHECK(a.delivery->working());
    a.delivery->stop();
}

TEST(a_job_set_aside_for_a_smaller_one_is_mined_again_and_finishes) {
    Side a("alice", "Alice"), b("bob", "Bob");
    // A search can find its nonce at any moment, so the medium job could be done before the tick
    // that sets it aside (it was, one run in twenty on the Windows runner, where that tick comes
    // 150 to 300 ms after it started): the search is held until the small job is queued and that
    // tick has come, and let go after it.
    std::optional<PowHeld> held{std::in_place};
    // 20 KB for an hour is about 2e7 trials: seconds on the slowest machines that run this.
    const auto medium = queueBroadcast(a, b, "Medium", 20000, 3600);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    CHECK(a.delivery->working());
    const auto small = queueBroadcast(a, b, "Small", 100, 3600);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    const auto jobState = [&](const std::string &owner) {
        for (const auto &j : a.mailbox.jobs())
            if (j.owner == owner)
                return j.state;
        return std::string("none");
    };
    CHECK_EQ(jobState(small), std::string("mining"));
    CHECK_EQ(jobState(medium), std::string("queued"));
    held.reset();
    const auto published = [&](const std::string &id) {
        return a.mailbox.outgoing(id).state == "published";
    };
    std::string first;
    // Both searches are random: the medium one takes its turn when the small one is done, and
    // takes as long as it takes, a few times its mean at worst.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
    while (!(published(small) && published(medium)) &&
           std::chrono::steady_clock::now() < deadline) {
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        if (first.empty())
            first = published(small) ? small : published(medium) ? medium : "";
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(published(small));
    CHECK(published(medium));
    CHECK_EQ(first, small); // the smaller one went first, though the other was being mined
    a.delivery->stop();
}

TEST(the_work_of_a_job_counts_the_life_of_its_object_and_not_only_its_size) {
    Side a("alice", "Alice"), b("bob", "Bob");
    // 100 bytes for 28 days is about 4e7 trials; 20 KB for an hour is about 2e7: the bigger object
    // is the cheaper job, and it goes first though the other was being mined. The search is held
    // until the tick that sets the long one aside has come, as in the test of the job set aside.
    std::optional<PowHeld> held{std::in_place};
    const auto longLived = queueBroadcast(a, b, "Long", 100, 28 * 86400);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    CHECK(a.delivery->working());
    const auto bigger = queueBroadcast(a, b, "Bigger", 20000, 3600);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    held.reset();
    const auto published = [&](const std::string &id) {
        return a.mailbox.outgoing(id).state == "published";
    };
    std::string first;
    // The long one is mined when the bigger one is done, and takes as long as it takes: a few times
    // its mean at worst, and the mean is 20 s on the Windows runner.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
    while (!(published(longLived) && published(bigger)) &&
           std::chrono::steady_clock::now() < deadline) {
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        if (first.empty())
            first = published(bigger) ? bigger : published(longLived) ? longLived : "";
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(published(longLived));
    CHECK(published(bigger));
    CHECK_EQ(first, bigger);
    a.delivery->stop();
}

namespace {
// A solver that only counts the searches it is given (one for each start() of the proof of work)
// and waits to be cancelled.
class CountingSolver : public bmc::PowSolver {
  public:
    std::atomic<int> searched{0};
    std::optional<uint64_t> search(const unsigned char *, uint64_t, uint64_t,
                                   const std::atomic_bool &cancel) override {
        ++searched;
        while (!cancel)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return std::nullopt;
    }
    void disable(const std::string &) override {}
};
// The solver is the proof of work's for as long as this lives.
struct SolverInstalled {
    explicit SolverInstalled(std::shared_ptr<bmc::PowSolver> solver) {
        bmc::ProofOfWork::setSolverEnabled(true);
        bmc::ProofOfWork::setSolver(std::move(solver));
    }
    ~SolverInstalled() {
        bmc::ProofOfWork::setSolver(nullptr);
    }
};
} // namespace

TEST(a_job_being_mined_is_neither_restarted_nor_given_up_for_one_of_the_same_work) {
    const auto solver = std::make_shared<CountingSolver>();
    SolverInstalled installed(solver);
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto expires = kNow() + 3600;
    // 1000 times the work per byte, so nothing here is ever solved.
    const auto add = [&](const char *kind, const char *address) {
        bmc::NetworkJob job;
        job.kind = kind;
        job.address = address;
        job.state = "queued";
        job.payload = bmc::Wire::acknowledgment(bmc::Bytes(32, 'p'), expires);
        job.expires = expires;
        job.nonceTrials = 1000000;
        job.extraBytes = 1000000;
        a.mailbox.addJob(job);
    };
    const auto turns = [&](int n) {
        for (int i = 0; i < n; ++i) {
            a.delivery->tick(a.mailbox, a.vault, true, kNow());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    add("message", "first");
    turns(40);
    CHECK_EQ(solver->searched.load(), 1); // started once, not again at every turn
    // A second job of the same work, which the order of jobs() lists first (a public key goes
    // before a message): the one that is being mined keeps its place.
    add("pubkey", "second");
    turns(40);
    CHECK_EQ(solver->searched.load(), 1);
    a.delivery->stop();
}

TEST(the_work_a_recipient_asks_for_counts_in_the_order_of_mining) {
    Side a("alice", "Alice"), b("bob", "Bob");
    // Two objects of the same size and life. The first is for a recipient that asks for 1000 times
    // the work per byte (the most the protocol lets a recipient ask for), which is minutes of work
    // for any machine that runs this; the second asks for the least.
    const auto expires = kNow() + 3600;
    const auto add = [&](const char *address, uint64_t trials, uint64_t extra) {
        bmc::NetworkJob job;
        job.kind = "pubkey";
        job.address = address;
        job.state = "queued";
        job.payload = bmc::Wire::acknowledgment(bmc::Bytes(32, 'p'), expires);
        job.expires = expires;
        job.nonceTrials = trials;
        job.extraBytes = extra;
        a.mailbox.addJob(job);
    };
    add("hard", 1000000, 1000000);
    a.delivery->tick(a.mailbox, a.vault, true, kNow());
    CHECK(a.delivery->working());
    add("easy", 1000, 1000);
    const auto done = [&] {
        return a.mailbox.setting("pubkey:easy", "") == std::to_string(expires);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(done());
    CHECK_EQ(a.mailbox.setting("pubkey:hard", ""), std::string());
    CHECK(a.delivery->working()); // the hard one is mined again
    a.delivery->stop();
}

TEST(a_letter_reaches_the_other_side_and_its_acknowledgment_comes_back) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Hello Bob", "Private body");
    a.mailbox.queueDraft(id, "direct", kNow() + 3600);
    // The key round trip is what the first rounds are for: without it the letter never leaves.
    a.delivery->tick(a.mailbox, a.vault, false, kNow());
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("awaiting_pubkey"));

    CHECK(pump(a, b, [&] { return a.mailbox.outgoing(id).state == "acknowledged"; }));
    CHECK_EQ(a.mailbox.message(id).folder, std::string("Sent"));
    CHECK(!a.mailbox.outgoing(id).objectHash.empty());

    const auto received = b.mailbox.messages();
    CHECK_EQ(received.size(), size_t(1));
    CHECK_EQ(received[0].body, std::string("Private body"));
    CHECK_EQ(received[0].subject, std::string("Hello Bob"));
    CHECK_EQ(received[0].from, a.identity);
    CHECK_EQ(received[0].folder, std::string("Inbox"));

    // A rescan over the same objects stores nothing twice.
    b.mailbox.advance(0);
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.messages().size(), size_t(1));
}

TEST(a_cancelled_letter_loses_its_jobs_and_a_retry_queues_it_again) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Hello Bob", "Body");
    a.mailbox.queueDraft(id, "direct", kNow() + 3600);
    a.delivery->tick(a.mailbox, a.vault, false, kNow());
    CHECK_EQ(a.mailbox.jobs().size(), size_t(1));

    a.delivery->cancel(a.mailbox, id);
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("cancelled"));
    const auto jobs = a.mailbox.jobs();
    CHECK(jobs.empty() || jobs[0].owner != id);

    a.delivery->retry(a.mailbox, id);
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("queued"));
    CHECK_EQ(a.mailbox.checkpoint(), int64_t(0));
}

TEST(a_subscribed_broadcast_is_stored_after_an_oversized_object) {
    Side a("alice", "Alice"), b("bob", "Bob");
    b.mailbox.subscribe(a.identity, "Alice's announcements");
    // A peer's object larger than the protocol allows: skipped, and the broadcast stored after it
    // is still read.
    fake::cacheObject(b.root, std::vector<uint8_t>(size_t(NTB_OBJECT_DB_MAX_OBJECT_SIZE) + 1, 'z'));
    fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(a.vault.identities()[0], "Announcement",
                                                         "Subscriber only", kNow() + 3600));

    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    bool found = false;
    for (const auto &m : b.mailbox.messages())
        if (m.folder == "Broadcasts" && m.subject == "Announcement")
            found = true;
    CHECK(found);
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(2));
}

TEST(a_broadcast_already_passed_is_found_by_the_catch_up) {
    Side a("alice", "Alice"), b("bob", "Bob");
    fake::cacheObject(b.root,
                      bmc::Wire::encodeBroadcast(a.vault.identities()[0], "Late announcement",
                                                 "Subscriber only", kNow() + 3600));
    // Without the subscription the scan reads the object and passes over it.
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(1));
    CHECK_EQ(b.mailbox.messages().size(), size_t(0));

    // Subscribing reads the kept objects again, without moving the checkpoint.
    b.mailbox.subscribe(a.identity, "Alice's announcements");
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    const auto messages = b.mailbox.messages();
    CHECK_EQ(messages.size(), size_t(1));
    CHECK_EQ(messages[0].subject, std::string("Late announcement"));
    CHECK_EQ(messages[0].folder, std::string("Broadcasts"));
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(1));
}

// A pending subscription listens without being listed: the first broadcast of its address is what
// makes it a subscription the user sees.
TEST(the_first_broadcast_of_a_pending_subscription_is_stored_and_lists_it) {
    Side a("alice", "Alice"), b("bob", "Bob");
    CHECK(b.mailbox.watch(a.identity));
    CHECK(b.mailbox.subscriptions().empty());
    fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(a.vault.identities()[0], "News", "Hello",
                                                         kNow() + 3600));

    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    bool found = false;
    for (const auto &m : b.mailbox.messages())
        if (m.folder == "Broadcasts" && m.subject == "News")
            found = true;
    CHECK(found);
    const auto listed = b.mailbox.subscriptions();
    CHECK_EQ(listed.size(), size_t(1));
    CHECK_EQ(listed[0].address, a.identity);
    CHECK_EQ(listed[0].label, std::string()); // no name of its own: the address book names it
    CHECK_EQ(b.mailbox.watched(), std::vector<std::string>{a.identity});
}

TEST(a_pending_subscription_that_has_not_posted_stays_unlisted) {
    Side a("alice", "Alice"), b("bob", "Bob"), c("carol", "Carol");
    CHECK(b.mailbox.watch(a.identity));
    // Somebody else's broadcast, and a letter: neither is Alice's.
    fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(c.vault.identities()[0], "News", "Hello",
                                                         kNow() + 3600));
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK(b.mailbox.messages().empty());
    CHECK(b.mailbox.subscriptions().empty());
    CHECK_EQ(b.mailbox.watched(), std::vector<std::string>{a.identity});
}

TEST(a_broadcast_already_passed_lists_a_pending_subscription_through_the_catch_up) {
    Side a("alice", "Alice"), b("bob", "Bob");
    fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(a.vault.identities()[0], "Late news",
                                                         "Hello", kNow() + 3600));
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(1));
    CHECK(b.mailbox.messages().empty());

    // The person writes: the address is watched, and the kept broadcasts are read again.
    CHECK(b.mailbox.watch(a.identity));
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    const auto messages = b.mailbox.messages();
    CHECK_EQ(messages.size(), size_t(1));
    CHECK_EQ(messages[0].subject, std::string("Late news"));
    CHECK_EQ(messages[0].folder, std::string("Broadcasts"));
    CHECK_EQ(b.mailbox.subscriptions().size(), size_t(1));
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(1));
}

// Every person the user deals with is watched, so a mailbox may listen for thousands of addresses:
// a broadcast is matched to its address by its tag, and not tried against each one.
TEST(a_broadcast_is_found_among_thousands_of_watched_addresses) {
    Side a("alice", "Alice"), b("bob", "Bob");
    {
        bmc::Mailbox::Batch batch(b.mailbox);
        for (uint32_t n = 0; n < 8000; ++n)
            b.mailbox.watch(anAddress(n));
        batch.commit();
    }
    CHECK(b.mailbox.watch(a.identity));
    constexpr int kPosts = 100;
    for (int i = 0; i < kPosts; ++i)
        fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(a.vault.identities()[0],
                                                             "Post " + std::to_string(i), "Hello",
                                                             kNow() + 3600));

    const auto began = std::chrono::steady_clock::now();
    while (b.mailbox.checkpoint() < kPosts)
        b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow(), kPosts);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    size_t posts = 0;
    for (const auto &m : b.mailbox.messages())
        if (m.folder == "Broadcasts")
            ++posts;
    CHECK_EQ(posts, size_t(kPosts));
    CHECK_EQ(b.mailbox.subscriptions().size(), size_t(1));
    // Trying each of 8,000 addresses for each of the 100 broadcasts took half a minute.
    CHECK(took < 5000);
}

TEST(the_watch_follows_the_subscriptions_as_they_come_and_go) {
    Side a("alice", "Alice"), b("bob", "Bob"), c("carol", "Carol");
    const auto post = [&](Side &from, const char *subject) {
        fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(from.vault.identities()[0], subject,
                                                             "Hello", kNow() + 3600));
        b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    };
    const auto stored = [&](const char *subject) {
        for (const auto &m : b.mailbox.messages())
            if (m.folder == "Broadcasts" && m.subject == subject)
                return true;
        return false;
    };
    b.mailbox.watch(a.identity);
    post(a, "One");
    CHECK(stored("One"));
    post(c, "Two"); // Carol is nobody's concern yet
    CHECK(!stored("Two"));
    // Carol joins the set, which Alice stays in: both are read.
    b.mailbox.watch(c.identity);
    post(c, "Three");
    post(a, "Four");
    CHECK(stored("Three") && stored("Four"));
    // Alice leaves it: only Carol is read.
    b.mailbox.unsubscribe(a.identity);
    post(a, "Five");
    post(c, "Six");
    CHECK(!stored("Five"));
    CHECK(stored("Six"));
}

TEST(a_message_to_a_service_address_is_filed_apart_and_counts_nowhere) {
    Side a("alice", "Alice"), b("bob", "Bob");
    // Two chans both sides have joined. To Bob, one of them is a service's: whatever comes to it is
    // not mail.
    const std::string service = a.vault.addChannel("the service chan of the delivery test", "", "");
    const std::string ordinary =
        a.vault.addChannel("an ordinary chan of the delivery test", "", "");
    CHECK_EQ(b.vault.addChannel("the service chan of the delivery test", "", ""), service);
    CHECK_EQ(b.vault.addChannel("an ordinary chan of the delivery test", "", ""), ordinary);
    b.delivery->setServiceAddresses({service});

    for (const auto &chan : {service, ordinary}) {
        const auto id =
            a.mailbox.saveDraft({}, a.identity, chan, "To the chan", "Everybody reads " + chan);
        a.mailbox.queueDraft(id, "direct", kNow() + 3600);
    }
    CHECK(pump(a, b, [&] { return b.mailbox.messages().size() == 2; }));

    std::map<std::string, std::string> folderOf; // by recipient
    for (const auto &m : b.mailbox.messages())
        folderOf[m.to] = m.folder;
    CHECK_EQ(folderOf[service], std::string("Service"));
    CHECK_EQ(folderOf[ordinary], std::string("Channels"));
    // The service chan is nowhere a person looks: not among the chans, not in the unread counts.
    CHECK_EQ(b.mailbox.channelAddresses("Channels"), std::vector<std::string>{ordinary});
    const auto unread = b.mailbox.unreadByFolder();
    CHECK_EQ(unread.size(), size_t(1));
    CHECK_EQ(unread.count("Channels"), size_t(1));
    CHECK_EQ(b.mailbox.unreadLetters(), 1);
    CHECK_EQ(b.mailbox.incomingLetterCount(), uint64_t(0));

    // A rescan stores nothing twice, with the setting or without it: the object is stored once, and
    // where it was first filed it stays.
    b.mailbox.advance(0);
    b.delivery->setServiceAddresses({});
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.messages().size(), size_t(2));
}

TEST(the_service_folder_is_taken_out_oldest_first_and_in_one_go) {
    Side b("bob", "Bob");
    for (int i = 0; i < 5; ++i)
        b.mailbox.store("service-" + std::to_string(i), "BM-sender", "BM-service", "",
                        "body " + std::to_string(i), -1, "Service");
    b.mailbox.store("mail", "BM-sender", b.identity, "hello", "mail", -1, "Inbox");
    const auto first = b.mailbox.takeService(3);
    CHECK_EQ(first.size(), size_t(3));
    CHECK(first[0].body == "body 0" && first[1].body == "body 1" && first[2].body == "body 2");
    CHECK_EQ(first[0].folder, std::string("Service"));
    const auto rest = b.mailbox.takeService(10);
    CHECK_EQ(rest.size(), size_t(2));
    CHECK_EQ(rest[1].body, std::string("body 4"));
    CHECK(b.mailbox.takeService(10).empty());
    // What was taken is gone for good, and nothing else was touched.
    CHECK_EQ(b.mailbox.messages().size(), size_t(1));
    CHECK_EQ(b.mailbox.messages()[0].folder, std::string("Inbox"));
    CHECK(b.mailbox.takeService(0).empty());
}

// The whole way of a lookup, as the app and a service make it: the app sends a chat from its
// service chan, the service (which has no key of that chan) answers to the chan's address, and what
// comes back is filed apart, not shown as a post of the chan.
TEST(a_chat_from_a_service_chan_is_answered_to_the_chan_and_the_answer_is_filed_apart) {
    Side app("app", "App"), service("service", "Service");
    const std::string chan =
        app.vault.addChannel("the chan of the lookups of the delivery test", "", "");
    app.delivery->setServiceAddresses({chan});
    app.mailbox.queueChat(chan, service.identity, "/address @ben", kNow() + 3600);
    // The service reads the question as a chat from the chan's address.
    CHECK(pump(app, service, [&] { return !service.mailbox.unreadChats(10).empty(); }));
    const auto chats = service.mailbox.unreadChats(10);
    CHECK_EQ(chats.size(), size_t(1));
    CHECK_EQ(chats[0].peerAddress, chan);
    CHECK_EQ(chats[0].localAddress, service.identity);
    CHECK_EQ(chats[0].body, std::string("/address @ben"));
    service.mailbox.markChatMessageRead(chats[0].id);
    // It answers to the address it was written from.
    service.mailbox.queueChat(service.identity, chan, "@ben is nobody.\nnickbot: {\"v\":1}",
                              kNow() + 3600);
    CHECK(pump(app, service, [&] { return !app.mailbox.messages().empty(); }));
    const auto messages = app.mailbox.messages();
    CHECK_EQ(messages.size(), size_t(1));
    CHECK_EQ(messages[0].folder, std::string("Service"));
    CHECK_EQ(messages[0].from, service.identity);
    CHECK_EQ(messages[0].to, chan);
    CHECK(app.mailbox.channelAddresses("Channels").empty());
    CHECK(app.mailbox.unreadByFolder().empty());
    // What the app does with it: the chat inside is the answer, and the folder is emptied.
    const auto taken = app.mailbox.takeService(10);
    CHECK_EQ(taken.size(), size_t(1));
    bmc::ChatPayload payload;
    CHECK(bmc::parseChatMessage(taken[0].subject, taken[0].body, &payload) ==
          bmc::ChatParseResult::Chat);
    CHECK_EQ(payload.body, std::string("@ben is nobody.\nnickbot: {\"v\":1}"));
    CHECK(app.mailbox.messages().empty());
}
