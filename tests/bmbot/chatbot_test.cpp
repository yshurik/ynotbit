// The chat bot behind chat_responder: durable, rate limited, bounded. The host is a fake with a
// mailbox's rules (unread chats, handed out once per run, read by done()); the clock is the test's
// own, in milliseconds.
#include "chat_bot.h"
#include <bmc/test.h>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using tools::ChatBot;
using tools::ChatBotOptions;
using tools::ChatHost;

namespace {
struct FakeHost : ChatHost {
    struct Row {
        std::string id, peer, text;
        bool read = false, handed = false;
    };
    std::string me = "BM-bot";
    std::vector<Row> rows;
    std::vector<std::vector<std::string>> sent; // peer, text, id
    std::map<std::string, std::string> states;
    int serial = 0, outSerial = 0;

    std::string arrive(const std::string &peer, const std::string &text) {
        rows.push_back({"in-" + std::to_string(++serial), peer, text});
        return rows.back().id;
    }
    void arriveMany(const std::string &peer, const std::string &text, int count) {
        for (int i = 0; i < count; ++i)
            arrive(peer, text + " " + std::to_string(i));
    }
    // A new run of the program: what was handed out and not done is handed out again.
    void newRun() {
        for (auto &row : rows)
            row.handed = false;
    }
    int unread() const {
        int n = 0;
        for (const auto &row : rows)
            n += !row.read;
        return n;
    }
    void markRead(const std::string &id) {
        for (auto &row : rows)
            if (row.id == id)
                row.read = true;
    }
    std::vector<std::string> outgoing(const std::string &peer) const {
        std::vector<std::string> bodies;
        for (const auto &s : sent)
            if (s[0] == peer)
                bodies.push_back(s[1]);
        return bodies;
    }
    bool nextChat(Chat &chat) override {
        for (auto &row : rows)
            if (!row.read && !row.handed) {
                row.handed = true;
                chat = {row.id, me, row.peer, row.text};
                return true;
            }
        return false;
    }
    void done(const std::string &id) override {
        markRead(id);
    }
    bool send(const std::string &, const std::string &peer, const std::string &text,
              std::string &id, std::string &error) override {
        if (text.size() > 8192) {
            error = "Chat message must be between 1 and 8192 bytes";
            return false;
        }
        id = "out-" + std::to_string(++outSerial);
        sent.push_back({peer, text, id});
        states[id] = "queued"; // as a mailbox says of a chat it has just queued
        return true;
    }
    std::string state(const std::string &id) override {
        const auto found = states.find(id);
        return found == states.end() ? std::string() : found->second;
    }
};

const std::string alice = "BM-alice", bob = "BM-bob";

struct Log {
    std::vector<std::string> lines;
    ChatBot::Log sink() {
        return [this](const std::string &line) { lines.push_back(line); };
    }
    bool has(const std::string &part) const {
        for (const auto &line : lines)
            if (line.find(part) != std::string::npos)
                return true;
        return false;
    }
};

const ChatBot::Handler echo = [](const std::string &, const std::string &,
                                 const std::string &text) {
    if (text == "/none")
        return std::vector<std::string>{};
    if (text == "/three")
        return std::vector<std::string>{"one", "two", "three"};
    if (text == "/long")
        return std::vector<std::string>{std::string(9000, 'x')}; // too long for a chat
    if (text == "/throws")
        throw std::runtime_error("the handler broke");
    return std::vector<std::string>{"echo " + text};
};

ChatBotOptions quick() {
    ChatBotOptions o;
    o.delayMs = 0;
    o.gapMs = 0;
    return o;
}
} // namespace

TEST(a_chat_that_came_is_answered_once_and_read) {
    FakeHost host;
    Log log;
    ChatBot bot(host, echo, quick(), log.sink());
    bot.poll(1000);
    CHECK(host.outgoing(alice).empty() && bot.stats().received == 0);
    host.arrive(alice, "hello");
    bot.poll(1000);
    CHECK(host.outgoing(alice) == std::vector<std::string>{"echo hello"});
    CHECK_EQ(host.unread(), 0);
    CHECK_EQ(bot.stats().received, uint64_t(1));
    CHECK_EQ(bot.stats().answered, uint64_t(1));
    CHECK_EQ(bot.stats().working, 0);
    bot.poll(2000);
    CHECK_EQ(host.outgoing(alice).size(), size_t(1)); // and not answered again
    CHECK(log.has("hello"));
}

TEST(the_answer_comes_after_the_delay_and_the_chat_stays_unread_until_then) {
    FakeHost host;
    Log log;
    auto slow = quick();
    slow.delayMs = 1000;
    ChatBot bot(host, echo, slow, log.sink());
    host.arrive(alice, "later");
    bot.poll(5000);
    CHECK(host.outgoing(alice).empty() && host.unread() == 1 && bot.stats().working == 1);
    bot.poll(5999);
    CHECK(host.outgoing(alice).empty()); // not a moment early
    bot.poll(6000);
    CHECK_EQ(host.outgoing(alice).size(), size_t(1));
    CHECK(host.unread() == 0 && bot.stats().working == 0);
}

TEST(what_waited_while_the_bot_was_down_is_answered_and_history_is_left_alone) {
    FakeHost host;
    Log log;
    const auto history = host.arrive(bob, "old news");
    host.markRead(history);
    auto slow = quick();
    slow.delayMs = 1000;
    {
        ChatBot first(host, echo, slow, log.sink());
        host.arrive(bob, "while down");
        first.poll(10000); // taken up, not yet answered: the bot stops here
    }
    CHECK(host.outgoing(bob).empty() && host.unread() == 1);
    host.newRun();
    ChatBot second(host, echo, slow, log.sink());
    second.poll(20000);
    second.poll(21000);
    CHECK(host.outgoing(bob) == std::vector<std::string>{"echo while down"});
    CHECK_EQ(host.unread(), 0);
}

TEST(several_answers_go_in_a_row_and_in_order_and_the_chat_is_read_after_the_last) {
    FakeHost host;
    Log log;
    auto spaced = quick();
    spaced.gapMs = 300;
    ChatBot bot(host, echo, spaced, log.sink());
    host.arrive(alice, "/three");
    bot.poll(30000);
    CHECK(host.outgoing(alice).size() == 1 && host.unread() == 1); // the first at once
    bot.poll(30299);
    CHECK_EQ(host.outgoing(alice).size(), size_t(1)); // the next after the gap
    bot.poll(30300);
    CHECK(host.outgoing(alice).size() == 2 && host.unread() == 1);
    bot.poll(30600);
    CHECK(host.outgoing(alice) == (std::vector<std::string>{"one", "two", "three"}));
    CHECK_EQ(host.unread(), 0);
}

TEST(a_handler_with_nothing_to_say_or_one_that_throws_stops_nothing) {
    FakeHost host;
    Log log;
    ChatBot bot(host, echo, quick(), log.sink());
    host.arrive(alice, "/none");
    host.arrive(alice, "/throws");
    host.arrive(alice, "still here");
    bot.poll(40000);
    CHECK(host.outgoing(alice) == std::vector<std::string>{"echo still here"});
    CHECK(host.unread() == 0 && bot.stats().failed == 1 && log.has("the handler broke"));
}

TEST(a_peer_that_asks_for_more_than_its_rate_is_answered_up_to_it_and_the_rest_dropped) {
    FakeHost host;
    Log log;
    auto limited = quick();
    limited.rateBurst = 3;
    limited.ratePerMinute = 60;
    ChatBot bot(host, echo, limited, log.sink());
    for (int i = 0; i < 5; ++i)
        host.arrive(bob, "flood " + std::to_string(i));
    bot.poll(50000);
    CHECK_EQ(host.outgoing(bob).size(), size_t(3));
    CHECK(host.unread() == 0 && bot.stats().limited == 2 && log.has("too many"));
    host.arrive(alice, "someone else");
    bot.poll(50000);
    CHECK(!host.outgoing(alice).empty() && host.outgoing(alice).back() == "echo someone else");
    host.arrive(bob, "after two seconds");
    bot.poll(52000);
    CHECK_EQ(host.outgoing(bob).size(), size_t(4));
    CHECK_EQ(host.outgoing(bob).back(), std::string("echo after two seconds"));
}

TEST(all_peers_together_are_limited_too) {
    FakeHost host;
    Log log;
    auto limited = quick();
    limited.globalBurst = 2;
    limited.globalPerMinute = 60;
    ChatBot bot(host, echo, limited, log.sink());
    host.arrive(alice, "one of many");
    host.arrive(bob, "two of many");
    host.arrive(alice, "three of many");
    bot.poll(60000);
    CHECK_EQ(host.outgoing(alice).size() + host.outgoing(bob).size(), size_t(2));
    CHECK(bot.stats().limited >= 1 && host.unread() == 0);
}

TEST(an_answer_that_will_not_queue_is_tried_again_and_then_given_up_and_the_chat_read) {
    FakeHost host;
    Log log;
    auto patient = quick();
    patient.retryMs = 1000;
    patient.maxAttempts = 3;
    ChatBot bot(host, echo, patient, log.sink());
    host.arrive(alice, "/long");
    bot.poll(70000);
    CHECK(host.unread() == 1 && bot.stats().working == 1 && bot.stats().failed == 0);
    bot.poll(70999);
    bot.poll(71000);
    CHECK(host.unread() == 1 && bot.stats().failed == 0); // twice
    bot.poll(72000);
    CHECK(host.unread() == 0 && bot.stats().failed == 1 && bot.stats().working == 0);
    CHECK(log.has("cannot answer"));
}

TEST(memory_is_bounded_a_thousand_chats_waiting_are_not_a_thousand_jobs) {
    FakeHost host;
    Log log;
    auto slow = quick();
    slow.delayMs = 100000;
    slow.batch = 20;
    {
        ChatBot bot(host, echo, slow, log.sink());
        host.arriveMany(alice, "backlog", 120);
        bot.poll(80000);
        CHECK(bot.stats().working == 20 && host.unread() == 120); // the rest wait in the mailbox
        bot.poll(80000 + 100000);
        CHECK_EQ(host.outgoing(alice).size(), size_t(20));
        CHECK(bot.stats().working == 20 && host.unread() == 100);
    }
    // The rest, quickly, as a bot with no delay would, following ten answers at a time.
    host.newRun();
    auto fastOptions = quick();
    fastOptions.track = 10;
    fastOptions.ratePerMinute = 0; // one peer sent them all: no limit, to see the rest
    fastOptions.globalPerMinute = 0;
    ChatBot fast(host, echo, fastOptions, log.sink());
    for (int i = 0; i < 10 && host.unread() > 0; ++i)
        fast.poll(300000 + i);
    CHECK(host.unread() == 0 && fast.stats().working == 0);
    CHECK_EQ(fast.stats().answered, uint64_t(100));
    CHECK(fast.stats().tracked <= 10 + 1); // what the bot remembers stays small
}

TEST(where_an_answer_got_to_is_said_as_it_changes_and_forgotten_when_final) {
    FakeHost host;
    Log log;
    ChatBot bot(host, echo, quick(), log.sink());
    host.arrive(bob, "state check");
    bot.poll(400000);
    CHECK(bot.stats().tracked >= 1); // an answer on its way is tracked
    const auto replyId = host.sent.back()[2];
    host.states[replyId] = "awaiting_ack";
    bot.poll(401000);
    CHECK(log.has("awaiting ack")); // underscores are spaces in the log
    host.states[replyId] = "acknowledged";
    bot.poll(402000);
    CHECK(log.has("acknowledged"));
    const auto tracked = bot.stats().tracked;
    bot.poll(403000);
    CHECK(bot.stats().tracked < tracked + 1 && bot.stats().tracked <= 2);
}

TEST(addresses_and_messages_are_shortened_for_a_log) {
    CHECK_EQ(ChatBot::brief("BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"), std::string("BM-2cVdr5sKJ…"));
    CHECK_EQ(ChatBot::oneLine("  two\n\tlines   and   gaps "), std::string("two lines and gaps"));
    const auto longLine = ChatBot::oneLine(std::string(200, 'a'));
    CHECK_EQ(longLine, std::string(79, 'a') + "…");
    // Characters, not bytes: a cut never falls inside a UTF-8 sequence.
    std::string cyrillic;
    for (int i = 0; i < 100; ++i)
        cyrillic += "я";
    const auto cut = ChatBot::oneLine(cyrillic);
    CHECK_EQ(cut.size(), size_t(79 * 2 + 3));
}
