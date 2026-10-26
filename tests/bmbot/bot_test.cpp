// The bot runtime on real vaults and mailboxes. There is no node in most of these tests: the relay
// between two bots is the fake node of the core's tests (a directory with an objects.sqlite, and
// the spool copied across).
#include "../bmclient/fake_node.h"
#include "bot.h"
#include <atomic>
#include <bmc/clock.h>
#include <bmc/json.h>
#include <bmc/phrase.h>
#include <bmc/pybitmessage.h>
#include <bmc/test.h>
#include <chrono>
#include <csignal>
#include <ctime>
#include <functional>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using bmbot_lib::Bot;
using bmbot_lib::Options;

namespace {
Options optionsFor(const std::string &root, const std::string &phrase) {
    Options o;
    o.dataDir = root;
    o.phrase = phrase;
    o.secret = false; // the constant password: opening costs the same as with a derived one
    o.node.bin = "";  // no node here: the fake relay moves the objects
    o.stepMs = 20;
    return o;
}

std::string addressOf(const std::string &phrase) {
    return bmc::phrase::identity(phrase, "x", 0).address;
}

bmc::json::Value parse(const std::string &text) {
    const auto parsed = bmc::json::parse(text);
    CHECK(parsed.has_value());
    return *parsed;
}

// The sender keeps the recipient's public half, as a peer's key is kept once it is known: no
// getpubkey round trip, whose proof of work is meant for 28 days.
void keepKeyOf(Bot &sender, const std::string &recipientPhrase) {
    const auto identity = bmc::phrase::identity(recipientPhrase, "x", 0);
    const auto p = bmc::Wire::publicIdentity(bmc::IdentityRef(identity));
    sender.inspect([&](bmc::Client &client, bmc::Vault &) {
        client.mailbox().savePublicKey({p.address, p.signingKey, p.encryptionKey, p.nonceTrials,
                                        p.extraBytes, p.behaviors, bmc::now() + 28 * 86400});
    });
}

// The relay between two data directories, until `done` or the deadline. The bots' own threads do
// the rest.
bool pump(const std::string &a, const std::string &b, const std::function<bool()> &done,
          int ms = 120000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    // `done` may take an event with it (a bot hands one out once): ask once a round.
    bool finished = done();
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        fake::deliver(a, b, bmc::now());
        fake::deliver(b, a, bmc::now());
        finished = done();
        if (!finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return finished;
}

// The next chat or letter, leaving notes aside (and keeping them, to say what went wrong).
bool nextEvent(Bot &bot, int ms, bmc::json::Value &event,
               std::vector<std::string> *notes = nullptr) {
    for (;;) {
        std::string json;
        if (bot.next(ms, json) != 0)
            return false;
        auto parsed = parse(json);
        if (parsed.at("kind").toString() != "note") {
            event = parsed;
            return true;
        }
        if (notes)
            notes->push_back(parsed.at("text").toString());
        ms = 0;
    }
}

void injectChat(Bot &bot, const std::string &id, const std::string &peer, const std::string &text,
                int64_t seqno = 1) {
    bot.inspect([&](bmc::Client &client, bmc::Vault &) {
        client.mailbox().storeChat(id, peer, bot.address(), seqno, text, bmc::now() + 3600, false);
    });
}
} // namespace

TEST(a_chat_crosses_between_two_bots_and_is_answered) {
    const auto dir = bmc::test::tempDir();
    const auto rootA = dir + "/alice", rootB = dir + "/bob";
    Bot a(optionsFor(rootA, "bot test alice")), b(optionsFor(rootB, "bot test bob"));
    CHECK_EQ(a.address(), addressOf("bot test alice"));
    keepKeyOf(a, "bot test bob");
    const auto sent = a.sendChat("", b.address(), "hello bob");
    CHECK(!sent.empty());

    bmc::json::Value event;
    std::vector<std::string> notes;
    CHECK(pump(rootA, rootB, [&] { return nextEvent(b, 0, event, &notes); }));
    CHECK_EQ(event.at("kind").toString(), std::string("chat"));
    CHECK_EQ(event.at("text").toString(), std::string("hello bob"));
    CHECK_EQ(event.at("from").toString(), a.address());
    CHECK_EQ(event.at("to").toString(), b.address());
    b.done(event.at("id").toString());
    bmc::json::Value none;
    CHECK(!nextEvent(b, 0, none)); // read, and not handed out again

    // The acknowledgment travels back: the sender follows its chat to the end.
    CHECK(pump(rootA, rootB, [&] { return a.state(sent) == "acknowledged"; }));

    // Bob kept Alice's key from the envelope he read: his answer needs no round trip.
    b.sendChat("", a.address(), "hello back");
    CHECK(pump(rootA, rootB, [&] { return nextEvent(a, 0, event, &notes); }));
    CHECK_EQ(event.at("text").toString(), std::string("hello back"));
    CHECK(notes.empty());
}

TEST(a_letter_crosses_and_is_read_by_done) {
    const auto dir = bmc::test::tempDir();
    const auto rootA = dir + "/alice", rootB = dir + "/bob";
    Bot a(optionsFor(rootA, "bot test alice")), b(optionsFor(rootB, "bot test bob"));
    keepKeyOf(a, "bot test bob");
    const auto sent = a.sendLetter("", b.address(), "Hello Bob", "A private body");
    bmc::json::Value event;
    CHECK(pump(rootA, rootB, [&] { return nextEvent(b, 0, event); }));
    CHECK_EQ(event.at("kind").toString(), std::string("letter"));
    CHECK_EQ(event.at("subject").toString(), std::string("Hello Bob"));
    CHECK_EQ(event.at("text").toString(), std::string("A private body"));
    CHECK_EQ(event.at("from").toString(), a.address());
    b.done(event.at("id").toString());
    bmc::json::Value none;
    CHECK(!nextEvent(b, 0, none));
    CHECK(pump(rootA, rootB, [&] { return a.state(sent) == "acknowledged"; }));
    // A letter that is refused leaves nothing behind in Drafts.
    CHECK_THROWS(a.sendLetter("", "not an address", "s", "b"));
}

TEST(an_event_not_done_comes_back_after_a_restart_and_a_done_one_does_not) {
    const auto root = bmc::test::tempDir() + "/bob";
    const auto peer = addressOf("bot test alice");
    {
        Bot b(optionsFor(root, "bot test bob"));
        injectChat(b, "in-1", peer, "hi");
        bmc::json::Value event;
        CHECK(nextEvent(b, 2000, event));
        CHECK_EQ(event.at("id").toString(), std::string("in-1"));
        CHECK_EQ(event.at("text").toString(), std::string("hi"));
        CHECK(!nextEvent(b, 100, event)); // once per run
    }
    {
        Bot b(optionsFor(root, "bot test bob")); // a new run: it is handed out again
        bmc::json::Value event;
        CHECK(nextEvent(b, 2000, event));
        CHECK_EQ(event.at("id").toString(), std::string("in-1"));
        b.done("in-1");
        b.done("in-1"); // harmless twice
        CHECK_THROWS(b.done("a stranger"));
        CHECK_THROWS(b.done(""));
    }
    Bot b(optionsFor(root, "bot test bob"));
    bmc::json::Value event;
    CHECK(!nextEvent(b, 200, event));
}

TEST(events_come_oldest_first_and_only_as_many_as_are_asked_for_at_once) {
    const auto root = bmc::test::tempDir() + "/bob";
    const auto peer = addressOf("bot test alice");
    Bot b(optionsFor(root, "bot test bob"));
    for (int i = 1; i <= 5; ++i)
        injectChat(b, "in-" + std::to_string(i), peer, "message " + std::to_string(i), i);
    for (int i = 1; i <= 5; ++i) {
        bmc::json::Value event;
        CHECK(nextEvent(b, 2000, event));
        CHECK_EQ(event.at("id").toString(), "in-" + std::to_string(i));
        CHECK_EQ(event.at("seqno").toInteger(), int64_t(i));
    }
    bmc::json::Value event;
    CHECK(!nextEvent(b, 50, event));
    for (int i = 1; i <= 5; ++i)
        b.done("in-" + std::to_string(i));
}

TEST(one_bot_holds_a_directory_and_only_its_phrase_opens_it) {
    const auto root = bmc::test::tempDir() + "/bob";
    Bot first(optionsFor(root, "bot test bob"));
    try {
        Bot second(optionsFor(root, "bot test bob"));
        CHECK(false);
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::State); // in use by another
    }
    first.close();
    try {
        Bot stranger(optionsFor(root, "another phrase"));
        CHECK(false);
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::Auth); // not the phrase this was made with
    }
    // A secret phrase makes the vault's password: another secret phrase cannot open it either.
    const auto secretRoot = bmc::test::tempDir() + "/secret";
    auto secret = optionsFor(secretRoot, "a secret phrase");
    secret.secret = true;
    { Bot made(secret); }
    secret.phrase = "a different secret";
    try {
        Bot stranger(secret);
        CHECK(false);
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::Auth);
    }
    Bot again(optionsFor(root, "bot test bob")); // the right phrase still opens it
    CHECK_EQ(again.address(), addressOf("bot test bob"));
}

TEST(closing_wakes_a_waiter_with_a_state_error) {
    Bot b(optionsFor(bmc::test::tempDir() + "/bob", "bot test bob"));
    std::atomic<int> code{0};
    std::thread waiter([&] {
        try {
            std::string event;
            b.next(60000, event);
        } catch (const bmc::Error &e) {
            code = int(e.code());
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto start = std::chrono::steady_clock::now();
    b.close();
    waiter.join();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK_EQ(code.load(), int(bmc::Code::State));
    CHECK_THROWS(b.status());
    b.close(); // idempotent
}

TEST(a_node_that_will_not_start_is_a_note_and_the_bot_still_opens) {
    auto o = optionsFor(bmc::test::tempDir() + "/bob", "bot test bob");
    o.node.bin = "/nonexistent/bmnode";
    o.node.publicNetwork = false;
    o.node.peer = "127.0.0.1:9";
    Bot b(o);
    std::string json;
    CHECK_EQ(b.next(3000, json), 0);
    const auto note = parse(json);
    CHECK_EQ(note.at("kind").toString(), std::string("note"));
    CHECK(note.at("text").toString().find("Node:") == 0);
    const auto status = parse(b.status());
    CHECK(!status.at("node").at("running").toBool(true));
    CHECK(status.at("node").at("managed").toBool());
    CHECK(status.at("network").toString().find("Node stopped") == 0);
}

TEST(the_status_says_what_the_bot_is) {
    Bot b(optionsFor(bmc::test::tempDir() + "/bob", "bot test bob"));
    const auto status = parse(b.status());
    CHECK_EQ(status.at("address").toString(), addressOf("bot test bob"));
    CHECK(status.at("time").toInteger() > 1'700'000'000);
    CHECK(status.at("uptime").toInteger() >= 0);
    CHECK(!status.at("node").at("managed").toBool(true)); // none is run here
    CHECK(status.at("up_to_date").isBool() && status.at("working").isBool());
    CHECK_EQ(status.at("node").at("peers").toInteger(-1), int64_t(0));
    CHECK_EQ(b.state("no such id"), std::string());
}

#ifdef BMBOT_TEST_NODE
TEST(the_node_runs_as_a_child_and_goes_with_the_bot) {
    auto o = optionsFor(bmc::test::tempDir() + "/bob", "bot test bob");
    o.node.bin = BMBOT_TEST_NODE;
    o.node.publicNetwork = false;
    o.node.peer = "127.0.0.1:9"; // nothing there: a private bot with no peer
    o.node.port = 0;
    int64_t pid = 0;
    {
        Bot b(o);
        bool written = false;
        for (int i = 0; i < 300 && !written; ++i) {
            const auto status = parse(b.status());
            pid = status.at("node").at("pid").toInteger();
            written = status.at("node").at("status_age").toInteger(-1) >= 0;
            if (!written)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(written); // the node wrote its status.json
        CHECK(pid > 0 && ::kill(pid_t(pid), 0) == 0);
        const auto status = parse(b.status());
        CHECK(status.at("node").at("running").toBool());
        CHECK_EQ(status.at("network").toString(),
                 std::string("No connected peers · waiting for network"));
    }
    bool gone = false;
    for (int i = 0; i < 50 && !gone; ++i) {
        gone = ::kill(pid_t(pid), 0) != 0;
        if (!gone)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(gone); // closing the bot stopped its node
}
#endif

#ifdef BMBOT_TEST_NODE
namespace {
// The real time, moved on by what a test says. The driver reads it through bmc::now(), and a node
// writes the real one: moving it on makes a status old and a restart due without waiting for them.
std::atomic<int64_t> skew{0};
int64_t skewedNow() {
    return int64_t(std::time(nullptr)) + skew.load();
}

Options privateNode(const std::string &name) {
    auto o = optionsFor(bmc::test::tempDir() + "/" + name, "bot test bob");
    o.node.bin = BMBOT_TEST_NODE;
    o.node.publicNetwork = false;
    o.node.peer = "127.0.0.1:9"; // nothing there
    return o;
}

// The node's pid once it has written a status; 0 if it does not within the time.
int64_t waitForNode(Bot &bot, int64_t not_pid = 0, int seconds = 30) {
    for (int i = 0; i < seconds * 10; ++i) {
        const auto node = parse(bot.status()).at("node");
        const auto pid = node.at("pid").toInteger();
        if (pid > 0 && pid != not_pid && node.at("status_age").toInteger(-1) >= 0)
            return pid;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

// A note whose text starts so, among the ones waiting; "" if none comes within the time.
std::string waitForNote(Bot &bot, const std::string &start, int seconds = 20) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string json;
        if (bot.next(200, json) != 0)
            continue;
        const auto event = parse(json);
        if (event.at("kind").toString() == "note" && event.at("text").toString().find(start) == 0)
            return event.at("text").toString();
    }
    return {};
}
} // namespace

TEST(a_node_that_dies_is_said_and_started_again_after_thirty_seconds) {
    skew = 0;
    bmc::setClockForTesting(skewedNow);
    Bot b(privateNode("bob"));
    const auto first = waitForNode(b);
    CHECK(first > 0);
    ::kill(pid_t(first), SIGKILL);
    CHECK(waitForNote(b, "The node stopped").find("crashed") != std::string::npos);
    skew = 31; // the restart is due thirty seconds after the exit
    const auto second = waitForNode(b, first);
    CHECK(second > 0 && second != first);
    CHECK(::kill(pid_t(second), 0) == 0);
    bmc::setClockForTesting(nullptr);
}

TEST(a_node_that_stops_writing_its_status_is_killed_and_started_again) {
    skew = 0;
    bmc::setClockForTesting(skewedNow);
    Bot b(privateNode("bob"));
    const auto first = waitForNode(b);
    CHECK(first > 0);
    // Frozen: it answers nothing, not even SIGTERM, so only the SIGKILL after three seconds ends
    // it.
    ::kill(pid_t(first), SIGSTOP);
    skew = 400; // its status is two minutes old and more, and the last start long ago
    CHECK(waitForNote(b, "Watchdog: restarted its node").find("its status stopped") !=
          std::string::npos);
    const auto second = waitForNode(b, first);
    CHECK(second > 0 && second != first);
    bool gone = false;
    for (int i = 0; i < 50 && !gone; ++i) {
        gone = ::kill(pid_t(first), 0) != 0; // the frozen one was killed, not left behind
        if (!gone)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(gone);
    CHECK_EQ(parse(b.status()).at("node").at("watchdog_restarts").toInteger(), int64_t(1));
    bmc::setClockForTesting(nullptr);
}
#endif

TEST(a_driver_that_stalls_is_noticed_by_the_hang_watch) {
    auto o = optionsFor(bmc::test::tempDir() + "/bob", "bot test bob");
    o.hangSeconds = 1;
    std::atomic<int> hangs{0};
    o.onHang = [&](int64_t stalled) {
        CHECK(stalled >= 1);
        ++hangs;
    };
    Bot b(o);
    CHECK_EQ(hangs.load(), 0);
    // Holding the lock stops the driver: its beat goes stale.
    b.inspect(
        [](bmc::Client &, bmc::Vault &) { std::this_thread::sleep_for(std::chrono::seconds(3)); });
    CHECK(hangs.load() >= 1);
}

TEST(the_configuration_is_checked) {
    const auto reason = [](const std::string &json) {
        try {
            Options::fromJson(json);
        } catch (const bmc::Error &e) {
            CHECK(e.code() == bmc::Code::Argument || e.code() == bmc::Code::Io);
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(reason("not json").find("JSON object") != std::string::npos);
    CHECK(reason("[]").find("JSON object") != std::string::npos);
    CHECK(reason("{}").find("data_dir") != std::string::npos);
    CHECK(reason(R"({"data_dir":"/x"})").find("phrase") != std::string::npos);
    CHECK(reason(R"({"data_dir":"/x","phrase":"p","node":{"bin":"","peer":"nonsense"}})")
              .find("node.peer") != std::string::npos);
    CHECK(reason(R"({"data_dir":"/x","phrase":"p","node":{"bin":"/b","public":false}})")
              .find("node.peer") != std::string::npos); // a private node needs its peer
    CHECK(reason(R"({"data_dir":"/x","phrase":"p","node":{"bin":"/b"}})").empty()); // public
    CHECK(reason(R"({"data_dir":"/x","phrase":"p"})").empty());
    CHECK(reason(R"({"data_dir":"/x","phrase":"p","node":{"bin":"","max_outgoing":65}})")
              .find("max_outgoing") != std::string::npos);
    CHECK(
        reason(R"({"data_dir":"/x","phrase":"p","node":{"bin":"","port":"8444"}})").find("port") !=
        std::string::npos);
    CHECK(reason(R"({"data_dir":"/x","phrase":"p","hang_seconds":-1})").find("hang_seconds") !=
          std::string::npos);
    CHECK(reason(R"({"data_dir":"/x","phrase_file":"/nonexistent/phrase"})").find("phrase file") !=
          std::string::npos);

    // A phrase file is its first line, trimmed, and secret; an inline phrase is public.
    const auto file = bmc::test::tempDir() + "/phrase.txt";
    const std::string content = "  a secret phrase \r\nsecond line\n";
    bmc::test::writeFile(file, bmc::view(content));
    const auto fromFile = Options::fromJson(R"({"data_dir":"/x","phrase_file":")" + file + R"("})");
    CHECK_EQ(fromFile.phrase, std::string("a secret phrase"));
    CHECK(fromFile.secret);
    const auto inlinePhrase = Options::fromJson(R"({"data_dir":"/x","phrase":"public"})");
    CHECK(!inlinePhrase.secret);
    CHECK(Options::fromJson(R"({"data_dir":"/x","phrase":"p","secret":true})").secret);
    const auto node = Options::fromJson(
        R"({"data_dir":"/x","phrase":"p","hang_seconds":120,"node":{"public":true,"listen":true,)"
        R"("port":18444,"max_outgoing":12,"max_incoming":24,"watchdog_minutes":15,)"
        R"("retention_mb":512,"retention_days":4,"peer":" 127.0.0.1:8444 "}})");
    CHECK(node.node.publicNetwork && node.node.listen && node.hangSeconds == 120);
    CHECK(Options::fromJson(R"({"data_dir":"/x","phrase":"p"})").node.publicNetwork); // the default
    CHECK(!Options::fromJson(
               R"({"data_dir":"/x","phrase":"p","node":{"public":false,"peer":"127.0.0.1:8444"}})")
               .node.publicNetwork);
    CHECK(node.node.port == 18444 && node.node.maxOutgoing == 12 && node.node.maxIncoming == 24);
    CHECK(node.node.watchdogMinutes == 15 && node.node.retentionMB == 512 &&
          node.node.retentionDays == 4);
    CHECK_EQ(node.node.peer, std::string("127.0.0.1:8444"));
}

// --- a bot made from keys instead of a phrase -------------------------------------------------

namespace {
// The private keys of a phrase's first pair are what its address is made of: a bot opened from
// them has the phrase's address.
struct PhraseKeys {
    unsigned char sign[32], enc[32];
    explicit PhraseKeys(const std::string &phrase) {
        bmc::phrase::key(phrase, 0, sign);
        bmc::phrase::key(phrase, 1, enc);
    }
    std::string signHex() const {
        return bmc::hex(bmc::ByteView(sign, 32));
    }
    std::string encHex() const {
        return bmc::hex(bmc::ByteView(enc, 32));
    }
};

std::string keysFileWith(const std::string &content) {
    const auto path = bmc::test::tempDir() + "/keys.txt";
    bmc::test::writeFile(path, bmc::view(content));
    return path;
}

std::string configWithKeys(const std::string &root, const std::string &file) {
    return R"({"data_dir":)" + bmc::json::quote(root) + R"(,"keys_file":)" +
           bmc::json::quote(file) + R"(,"node":{"bin":""}})";
}

Options optionsWithKeys(const std::string &root, const std::string &file) {
    auto o = Options::fromJson(configWithKeys(root, file));
    o.stepMs = 20;
    return o;
}
} // namespace

TEST(a_bot_opened_from_a_keys_file_has_the_address_of_its_keys) {
    const PhraseKeys keys("bot test keys");
    const auto file = keysFileWith(keys.signHex() + "\n" + keys.encHex() + "\n");
    const auto o = optionsWithKeys(bmc::test::tempDir() + "/bot", file);
    CHECK(o.secret); // as with a phrase file, the keys make the vault's password
    CHECK(o.phrase.empty());
    Bot bot(o);
    CHECK_EQ(bot.address(), addressOf("bot test keys"));
    bot.inspect([&](bmc::Client &, bmc::Vault &vault) {
        CHECK_EQ(vault.identities().size(), size_t(1));
        CHECK_EQ(vault.identities().front().label, o.label); // "Bot", not the empty label of keys
    });
}

TEST(a_keys_file_holds_two_keys_as_hex_or_wallet_import_lines) {
    const PhraseKeys keys("bot test keys");
    const auto both = [](const Options &o) {
        return bmc::hex(bmc::view(o.signingKey)) + " " + bmc::hex(bmc::view(o.encryptionKey));
    };
    // Comments, empty lines, a BOM, CRLF and the space around a key are not part of it.
    const auto plain = Options::fromJson(
        configWithKeys("/x", keysFileWith("\xEF\xBB\xBF# the keys of a bot\r\n\r\n  " +
                                          keys.signHex() + "  \r\n" + keys.encHex() + "\r\n\r\n")));
    CHECK_EQ(both(plain), keys.signHex() + " " + keys.encHex());
    // PyBitmessage's keys.dat writes a private key in the wallet import format: a line may be one.
    const std::string wif = "5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ";
    const auto decoded = bmc::pybm::decodeWif(wif);
    const auto mixed =
        Options::fromJson(configWithKeys("/x", keysFileWith(wif + "\n" + keys.encHex() + "\n")));
    CHECK_EQ(both(mixed), bmc::hex(bmc::ByteView(decoded.data(), 32)) + " " + keys.encHex());
}

TEST(a_keys_file_that_is_wrong_is_refused_and_never_quoted) {
    const PhraseKeys keys("bot test keys");
    const auto reason = [](const std::string &json) {
        try {
            Options::fromJson(json);
        } catch (const bmc::Error &e) {
            CHECK(e.code() == bmc::Code::Argument || e.code() == bmc::Code::Io);
            return std::string(e.what());
        }
        return std::string();
    };
    const auto with = [&](const std::string &content) {
        return reason(configWithKeys("/x", keysFileWith(content)));
    };
    const auto has = [](const std::string &text, const char *part) {
        return text.find(part) != std::string::npos;
    };
    CHECK(has(reason(R"({"data_dir":"/x","keys_file":"/nonexistent/keys"})"), "keys file"));
    CHECK(has(with(""), "two private keys"));
    CHECK(has(with(keys.signHex() + "\n"), "two private keys"));
    CHECK(has(with(keys.signHex() + "\n" + keys.encHex() + "\n" + keys.encHex() + "\n"),
              "two private keys"));
    // A line that is no key says which one it is for, and never what it holds.
    const auto notHex = "zz" + keys.encHex().substr(2);
    const auto bad = with(keys.signHex() + "\n" + notHex + "\n");
    CHECK(has(bad, "encryption key"));
    CHECK(!has(bad, notHex.c_str()) && !has(bad, keys.signHex().c_str()));
    // Zero, and the curve's order, are no private keys.
    CHECK(has(with(std::string(64, '0') + "\n" + keys.encHex() + "\n"), "valid private key"));
    CHECK(has(with(keys.signHex() + "\nFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD"
                                    "0364141\n"),
              "valid private key"));
    // The address comes from one place.
    const auto file = keysFileWith(keys.signHex() + "\n" + keys.encHex() + "\n");
    const auto quoted = bmc::json::quote(file);
    CHECK(has(reason(R"({"data_dir":"/x","phrase":"p","keys_file":)" + quoted + "}"), "keys_file"));
    CHECK(has(reason(R"({"data_dir":"/x","phrase_file":"/p","keys_file":)" + quoted + "}"),
              "keys_file"));
}

TEST(a_bot_made_from_keys_opens_again_with_them_and_not_with_other_keys) {
    const auto root = bmc::test::tempDir() + "/bot";
    const PhraseKeys keys("bot test keys"), other("bot test other keys");
    const auto file = keysFileWith(keys.signHex() + "\n" + keys.encHex() + "\n");
    const auto otherFile = keysFileWith(other.signHex() + "\n" + other.encHex() + "\n");
    { Bot made(optionsWithKeys(root, file)); }
    {
        Bot again(optionsWithKeys(root, file));
        CHECK_EQ(again.address(), addressOf("bot test keys"));
    }
    // The vault's password is made from the keys, so other keys do not open it.
    bool refused = false;
    try {
        Bot wrong(optionsWithKeys(root, otherFile));
    } catch (const bmc::Error &e) {
        refused = e.code() == bmc::Code::Auth;
    }
    CHECK(refused);
}

TEST(
    a_data_directory_made_from_keys_is_not_opened_with_other_keys_even_with_the_constant_password) {
    // With "secret":false the password is the constant one, so only the address can tell.
    const auto root = bmc::test::tempDir() + "/bot";
    const PhraseKeys keys("bot test keys"), other("bot test other keys");
    const auto open = [&](const PhraseKeys &k) {
        const auto file = keysFileWith(k.signHex() + "\n" + k.encHex() + "\n");
        auto o =
            Options::fromJson(R"({"data_dir":)" + bmc::json::quote(root) + R"(,"keys_file":)" +
                              bmc::json::quote(file) + R"(,"secret":false,"node":{"bin":""}})");
        o.stepMs = 20;
        return o;
    };
    { Bot made(open(keys)); }
    std::string why;
    try {
        Bot wrong(open(other));
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::Auth);
        why = e.what();
    }
    CHECK(why.find("other keys") != std::string::npos);
}

TEST(a_bot_made_from_keys_sends_a_chat_that_a_bot_made_from_a_phrase_reads) {
    const auto dir = bmc::test::tempDir();
    const auto rootA = dir + "/alice", rootB = dir + "/bob";
    const PhraseKeys keys("bot test keys");
    Bot a(optionsWithKeys(rootA, keysFileWith(keys.signHex() + "\n" + keys.encHex() + "\n")));
    Bot b(optionsFor(rootB, "bot test bob"));
    keepKeyOf(a, "bot test bob");
    CHECK(!a.sendChat("", b.address(), "hello from keys").empty());
    bmc::json::Value event;
    CHECK(pump(rootA, rootB, [&] { return nextEvent(b, 0, event); }));
    CHECK_EQ(event.at("text").toString(), std::string("hello from keys"));
    CHECK_EQ(event.at("from").toString(), addressOf("bot test keys"));
}
