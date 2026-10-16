#pragma once
// A chat bot: answers the chats that come to a mailbox, and keeps on doing so for months. It is
// the engine of chat_responder, and what a bot or a service made on ynotbit's chats starts from.
//
// What has come and nobody has looked at is the queue, and the mailbox keeps it: a chat is marked
// read only once its answers are queued, so a bot that is stopped, crashes or loses its network
// answers what waited when it runs again (at least once: a crash between the last answer and the
// mark answers it twice). What it holds in memory is bounded however long it runs and however
// many chats there are; a peer cannot make it answer without limit.
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
namespace bm {
class Session;
}
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
class ChatBot {
  public:
    // What it says to a chat: its answers, in order, to text from peer to the identity local.
    // None says nothing; one that throws is logged and dropped, never stops the bot.
    using Handler =
        std::function<QStringList(const QString &local, const QString &peer, const QString &text)>;
    // Where it says what it does: one line each.
    using Log = std::function<void(const QString &)>;
    ChatBot(bm::Session &session, Handler handler, ChatBotOptions options = {}, Log log = {});
    // Takes up the chats that came and sends the answers that are due. now is milliseconds,
    // from any steady clock. Cheap when there is nothing to do: call it several times a second.
    void poll(qint64 now);
    struct Stats {
        quint64 received = 0; // chats taken up
        quint64 answered = 0; // of them answered in full
        quint64 limited = 0;  // answers left unsent because a peer, or all, asked for too many
        quint64 failed = 0;   // chats given up: an answer would not queue, or the handler threw
        int working = 0;      // chats held now
        int tracked = 0;      // what else it remembers: peers' rates, answers on their way
    };
    Stats stats() const;
    // An address, and a message on one line, short enough for a log.
    static QString brief(const QString &address);
    static QString oneLine(const QString &text);

  private:
    struct Job {
        QString id, local, peer;
        QStringList replies;
        int next = 0;     // the answer to send next
        qint64 due = 0;   // when
        int attempts = 0; // of the answer being sent
    };
    struct Bucket {
        double tokens = 0;
        qint64 last = 0;
        bool said = false;      // it has said the peer was held back...
        qint64 saidLimited = 0; // ...and when, to say it once a minute
    };
    struct Sent {
        QString peer, state;
    };
    bm::Session &session_;
    Handler handler_;
    ChatBotOptions options_;
    Log log_;
    QHash<QString, Job> jobs_; // by the id of the chat answered
    QList<QString> order_;     // their ids, oldest first
    QHash<QString, Bucket> peers_;
    Bucket all_;
    bool allStarted_ = false;
    QHash<QString, Sent> sent_; // answers on their way, by id
    QList<QString> sentOrder_;  // their ids, oldest first
    qint64 nextStates_ = 0, nextPrune_ = 0;
    Stats stats_;
    void say(const QString &line) const;
    void admit(qint64 now);
    void send(qint64 now);
    void watchStates(qint64 now);
    int allowed(const QString &peer, int wanted, qint64 now);
    void finish(const QString &id);
};
} // namespace tools
