#pragma once
// A chat bot: answers the chats that come to a bot's mailbox, and keeps on doing so for months. It
// is the engine of chat_responder, and what a bot or a service made on ynotbit's chats starts from.
//
// What has come and nobody has looked at is the queue, and the mailbox keeps it: a chat is marked
// read only once its answers are queued, so a bot that is stopped, crashes or loses its network
// answers what waited when it runs again (at least once: a crash between the last answer and the
// mark answers it twice). What it holds in memory is bounded however long it runs and however
// many chats there are; a peer cannot make it answer without limit.
//
// It knows its host only through ChatHost: bmbot's C ABI in chat_responder, a fake in the tests.
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tools {

struct ChatBotOptions {
    int delayMs = 1000;        // after a chat is taken up, before its first answer
    int gapMs = 300;           // between the answers to one chat: in a row, in order
    int batch = 50;            // chats held at once; the rest wait, unread, in the mailbox
    int rateBurst = 30;        // answers one peer can get at once...
    double ratePerMinute = 30; // ...and how many more it gets each minute; 0 sets no limit
    int globalBurst = 200;     // the same for all peers together, for a flood from many
    double globalPerMinute = 300;
    int retryMs = 5000;  // an answer that cannot be queued is tried again after this...
    int maxAttempts = 6; // ...this many times in all, and then given up
    int track = 200;     // answers followed to their end at once; older ones are let go
};

class ChatHost {
  public:
    struct Chat {
        std::string id, local, peer, text;
    };
    virtual ~ChatHost() = default;
    // The oldest chat nobody has looked at and this run has not handed out yet; false if none.
    virtual bool nextChat(Chat &chat) = 0;
    // Marks a chat as looked at.
    virtual void done(const std::string &id) = 0;
    // Queues an answer. False, with why in error, when it cannot be.
    virtual bool send(const std::string &local, const std::string &peer, const std::string &text,
                      std::string &id, std::string &error) = 0;
    // Where a chat that was sent got to ("acknowledged", ...); empty if unknown.
    virtual std::string state(const std::string &id) = 0;
};

class ChatBot {
  public:
    // What it says to a chat: its answers, in order, to text from peer to the identity local.
    // None says nothing; one that throws is logged and dropped, never stops the bot.
    using Handler = std::function<std::vector<std::string>(
        const std::string &local, const std::string &peer, const std::string &text)>;
    // Where it says what it does: one line each.
    using Log = std::function<void(const std::string &)>;
    ChatBot(ChatHost &host, Handler handler, ChatBotOptions options = {}, Log log = {});
    // Takes up the chats that came and sends the answers that are due. now is milliseconds, from
    // any steady clock. Cheap when there is nothing to do: call it several times a second.
    void poll(int64_t now);
    struct Stats {
        uint64_t received = 0; // chats taken up
        uint64_t answered = 0; // of them answered in full
        uint64_t limited = 0;  // answers left unsent because a peer, or all, asked for too many
        uint64_t failed = 0;   // chats given up: an answer would not queue, or the handler threw
        int working = 0;       // chats held now
        int tracked = 0;       // what else it remembers: peers' rates, answers on their way
    };
    Stats stats() const;
    // An address, and a message on one line, short enough for a log.
    static std::string brief(const std::string &address);
    static std::string oneLine(const std::string &text);

  private:
    struct Job {
        std::string id, local, peer;
        std::vector<std::string> replies;
        size_t next = 0;  // the answer to send next
        int64_t due = 0;  // when
        int attempts = 0; // of the answer being sent
    };
    struct Bucket {
        double tokens = 0;
        int64_t last = 0;
        bool said = false;       // it has said the peer was held back...
        int64_t saidLimited = 0; // ...and when, to say it once a minute
    };
    struct Sent {
        std::string peer, state;
    };
    ChatHost &host_;
    Handler handler_;
    ChatBotOptions options_;
    Log log_;
    std::unordered_map<std::string, Job> jobs_; // by the id of the chat answered
    std::deque<std::string> order_;             // their ids, oldest first
    std::unordered_map<std::string, Bucket> peers_;
    Bucket all_;
    bool allStarted_ = false;
    std::unordered_map<std::string, Sent> sent_; // answers on their way, by id
    std::deque<std::string> sentOrder_;          // their ids, oldest first
    int64_t nextStates_ = 0, nextPrune_ = 0;
    Stats stats_;
    void say(const std::string &line) const;
    void admit(int64_t now);
    void send(int64_t now);
    void watchStates(int64_t now);
    int allowed(const std::string &peer, int wanted, int64_t now);
    void finish(const std::string &id);
};

} // namespace tools
