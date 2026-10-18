// The bot runtime under the C ABI (bmbot.h): a vault and a mailbox opened from a phrase, the node
// as a child process, and a driver thread that does what the desktop's Session::tick does -- watch
// the node, step the client -- so that a service needs no event loop of its own. Everything that
// touches the vault, the mailbox or the node runs under one mutex: their SQLite handles belong to
// one thread at a time.
#pragma once
#include <atomic>
#include <bmc/client.h>
#include <bmc/node_config.h>
#include <bmc/node_status.h>
#include <bmc/node_supervisor.h>
#include <bmc/node_watch.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>

namespace bmbot_lib {

struct Options {
    std::string dataDir;
    std::string phrase;
    std::string label = "Bot"; // the label of the identity made from the phrase
    // A secret phrase also makes the vault's password (a hash of it), so what is on the disk is of
    // no use without it. A phrase that is public keeps the constant password of the first
    // chat_responder, whose folders it opens.
    bool secret = true;
    struct Node {
        // "auto": $BMBOT_NODE, then bmnode beside the library or the program, then the PATH.
        // "": no node is run here, one that is run elsewhere on dataDir is assumed.
        std::string bin = "auto";
        std::string peer, proxy;
        bool publicNetwork = true; // false: the node links to its peer alone (-b -B -e)
        bool listen = false;
        int port = 0, maxOutgoing = 0, maxIncoming = 0;
        int watchdogMinutes = 0; // restart a node that had no peer that long; 0 never
        int retentionMB = 2048, retentionDays = 90;
    } node;
    // The process aborts when the driver loop has not run for this long, so that Docker or systemd
    // starts it again; 0 never.
    int hangSeconds = 0;
    int stepMs = 300;                    // the driver's pace once it is up to date
    std::function<void(int64_t)> onHang; // instead of abort(); for tests
    // Reads the JSON the C ABI takes. Throws bmc::Error(Argument) with the reason.
    static Options fromJson(std::string_view text);
};

class Bot {
  public:
    // Opens (or, for a new data directory, makes) the vault and the mailbox, starts the node and
    // the driver. Throws bmc::Error: State when another process holds the directory, Auth for a
    // phrase that is not the one the vault was made with.
    explicit Bot(Options options);
    ~Bot();
    Bot(const Bot &) = delete;
    Bot &operator=(const Bot &) = delete;

    // 0 with the next event in eventJson, 1 when none came within timeoutMs. Notes first, then
    // the oldest chat or letter not yet handed out in this run. Throws State once closed.
    int next(int timeoutMs, std::string &eventJson);
    // Marks the event's chat or letter as read; what was not done comes back after a restart.
    void done(const std::string &id);
    // from empty: the default identity. Return the id to follow with state().
    std::string sendChat(const std::string &from, const std::string &to, const std::string &text);
    std::string sendLetter(const std::string &from, const std::string &to,
                           const std::string &subject, const std::string &body);
    // A delivery state of a chat or a letter, by its id; empty if there is none.
    std::string state(const std::string &id);
    std::string status();
    std::string address() const {
        return address_;
    }
    // Wakes the waiters, which get a State error, and stops the driver and the node. Idempotent.
    void close();
    // For tests and tools: runs f on the client under the lock.
    void inspect(const std::function<void(bmc::Client &, bmc::Vault &)> &f);

  private:
    Options o_;
    std::string root_, vaultPath_, mailPath_, address_;
    int lockFd_ = -1;
    std::shared_ptr<bmc::Vault> vault_;
    std::shared_ptr<bmc::Mailbox> mailbox_;
    std::unique_ptr<bmc::Client> client_;
    std::unique_ptr<bmc::NodeSupervisor> node_;
    bmc::NodeWatch watch_;
    std::optional<bmc::NodeStatus> status_;
    std::string nodeError_, lastStderr_, watchdogNote_;
    int64_t restartAt_ = 0, startedAt_ = 0;
    int nodeRestarts_ = 0, watchdogRestarts_ = 0;
    bool working_ = false, upToDate_ = false, prunedNoted_ = false;
    std::string lastProblem_;
    int64_t lastProblemAt_ = 0;

    mutable std::mutex m_;
    std::condition_variable eventCv_, driverCv_, idleCv_;
    bool stop_ = false, closing_ = false, closed_ = false, wake_ = false;
    int inflight_ = 0;
    std::deque<std::string> notes_;
    std::set<std::string> handedChats_, handedLetters_;
    std::thread driver_;

    std::mutex hangMutex_;
    std::condition_variable hangCv_;
    bool hangStop_ = false;
    std::atomic<int64_t> beat_{0};
    std::thread hang_;

    void lockDirectory();
    void openStorage();
    void startDriver();
    void drive();
    void tick();
    void pollNode(int64_t now);
    void startNode(int64_t now);
    void restartNode(int64_t now);
    void watchHang();
    void note(const std::string &text);
    void problem(const std::string &text);
    bool takeChat(std::string &eventJson);
    bool takeLetter(std::string &eventJson);
    std::string networkLine(int64_t now) const;
    std::string fromOrDefault(const std::string &from) const;
    void ensureOpen() const;
    void rememberDone(const std::string &id);
    std::set<std::string> recentDone_;
    std::deque<std::string> recentDoneOrder_;
};

} // namespace bmbot_lib
