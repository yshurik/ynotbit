#include "bot.h"
#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/error.h>
#include <bmc/hash.h>
#include <bmc/json.h>
#include <bmc/mailbox.h>
#include <bmc/path.h>
#include <bmc/phrase.h>
#include <bmc/text.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace bmbot_lib {
namespace {
using bmc::Code;
using bmc::Error;
namespace fs = std::filesystem;
using Value = bmc::json::Value;

constexpr size_t kMaxHanded = 10000; // events handed out and not done: a consumer that forgets
constexpr size_t kMaxNotes = 64;
constexpr size_t kRememberDone = 4096;
constexpr int64_t kNodeRetrySeconds =
    30; // what stopped a node (a busy port, a locked store) may clear

int64_t steadySeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string trimmed(std::string_view s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && space(s.back()))
        s.remove_suffix(1);
    return std::string(s);
}

// The first line of a file, as chat_responder read its phrase: a BOM and the white space around
// it are not part of the phrase.
std::string firstLine(std::string raw) {
    if (raw.rfind("\xEF\xBB\xBF", 0) == 0)
        raw.erase(0, 3);
    return trimmed(raw.substr(0, raw.find('\n')));
}

std::string number(int64_t n) {
    return std::to_string(n);
}
std::string quote(std::string_view s) {
    return bmc::json::quote(s);
}

// --- the JSON the library is opened with ------------------------------------------------------

std::string textOf(const Value &object, const char *key) {
    const auto &v = object.at(key);
    if (v.isUndefined() || v.isNull())
        return {};
    if (!v.isString())
        throw Error(Code::Argument, std::string(key) + " must be a string");
    return v.toString();
}
bool flagOf(const Value &object, const char *key, bool fallback) {
    const auto &v = object.at(key);
    if (v.isUndefined() || v.isNull())
        return fallback;
    if (!v.isBool())
        throw Error(Code::Argument, std::string(key) + " must be true or false");
    return v.toBool();
}
int wholeOf(const Value &object, const char *key, int fallback, int low, int high) {
    const auto &v = object.at(key);
    if (v.isUndefined() || v.isNull())
        return fallback;
    if (!v.isNumber() || v.toDouble() != std::floor(v.toDouble()))
        throw Error(Code::Argument, std::string(key) + " must be a whole number");
    const auto n = v.toInteger(low - 1);
    if (n < low || n > high)
        throw Error(Code::Argument, std::string(key) + " must be from " + std::to_string(low) +
                                        " to " + std::to_string(high));
    return int(n);
}

// --- finding bmnode ---------------------------------------------------------------------------

bool isExecutableFile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}
fs::path libraryDirectory() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&libraryDirectory), &info) && info.dli_fname)
        return fs::path(info.dli_fname).parent_path();
    return {};
}
fs::path executableDirectory() {
#ifdef __APPLE__
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0)
        return fs::path(buffer).parent_path();
    return {};
#else
    std::error_code ec;
    const auto link = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : link.parent_path();
#endif
}
std::string onPath(const std::string &name) {
    const char *path = std::getenv("PATH");
    std::stringstream parts(path ? path : "");
    std::string dir;
    while (std::getline(parts, dir, ':'))
        if (!dir.empty() && isExecutableFile(fs::path(dir) / name))
            return (fs::path(dir) / name).string();
    return {};
}
// "auto" looks in the places a program and its node are installed together; "" runs no node.
std::string findNode(const std::string &configured) {
    if (configured.empty())
        return {};
    if (configured.find('/') != std::string::npos)
        return configured;
    if (configured != "auto")
        return onPath(configured);
    if (const char *env = std::getenv("BMBOT_NODE"); env && *env)
        return env;
    // Beside the library or the program, or in the bin of the prefix that has the library's lib.
    for (const auto &dir : {libraryDirectory(), executableDirectory()})
        for (const auto &candidate : {dir / "bmnode", dir / ".." / "bin" / "bmnode"})
            if (!dir.empty() && isExecutableFile(candidate))
                return candidate.lexically_normal().string();
    return onPath("bmnode");
}
} // namespace

Options Options::fromJson(std::string_view text) {
    const auto parsed = bmc::json::parse(text);
    if (!parsed || !parsed->isObject())
        throw Error(Code::Argument, "The configuration must be a JSON object");
    const auto &c = *parsed;
    Options o;
    o.dataDir = textOf(c, "data_dir");
    if (o.dataDir.empty())
        throw Error(Code::Argument, "data_dir is required");
    const auto file = textOf(c, "phrase_file");
    o.phrase = textOf(c, "phrase");
    if (file.empty() && o.phrase.empty())
        throw Error(Code::Argument, "phrase_file or phrase is required");
    if (!file.empty()) {
        std::ifstream in(bmc::path(file), std::ios::binary);
        if (!in)
            throw Error(Code::Io, "Cannot read the phrase file");
        std::stringstream raw;
        raw << in.rdbuf();
        o.phrase = firstLine(raw.str());
        o.secret = true;
    } else {
        o.secret = false;
    }
    if (o.phrase.empty())
        throw Error(Code::Argument, "The phrase is empty");
    o.secret = flagOf(c, "secret", o.secret);
    if (const auto label = textOf(c, "label"); !label.empty())
        o.label = label;
    o.hangSeconds = wholeOf(c, "hang_seconds", 0, 0, 86400);
    o.stepMs = wholeOf(c, "step_ms", 300, 10, 5000);

    const auto &n = c.at("node");
    if (!n.isUndefined() && !n.isNull() && !n.isObject())
        throw Error(Code::Argument, "node must be an object");
    if (n.isObject()) {
        if (!n.at("bin").isUndefined())
            o.node.bin = textOf(n, "bin");
        o.node.peer = trimmed(textOf(n, "peer"));
        o.node.proxy = trimmed(textOf(n, "proxy"));
        if (!bmc::validEndpoint(o.node.peer))
            throw Error(Code::Argument, "node.peer must be IP:port");
        if (!bmc::validEndpoint(o.node.proxy))
            throw Error(Code::Argument, "node.proxy must be IP:port");
        o.node.publicNetwork = flagOf(n, "public", true);
        o.node.listen = flagOf(n, "listen", false);
        o.node.port = wholeOf(n, "port", 0, 0, 65535);
        o.node.maxOutgoing = wholeOf(n, "max_outgoing", 0, 0, 64);
        o.node.maxIncoming = wholeOf(n, "max_incoming", 0, 0, 512);
        o.node.watchdogMinutes = wholeOf(n, "watchdog_minutes", 0, 0, 24 * 60);
        o.node.retentionMB = wholeOf(n, "retention_mb", 2048, 64, 32768);
        o.node.retentionDays = wholeOf(n, "retention_days", 90, 1, 3650);
    }
    if (!o.node.bin.empty() && !o.node.publicNetwork && o.node.peer.empty())
        throw Error(Code::Argument, "node.peer is required unless node.public is true");
    return o;
}

// --- opening ----------------------------------------------------------------------------------

Bot::Bot(Options options) : o_(std::move(options)), root_(o_.dataDir) {
    std::error_code ec;
    fs::create_directories(bmc::path(root_), ec);
    if (ec || !fs::is_directory(bmc::path(root_)))
        throw Error(Code::Io, "Cannot create the data directory");
    fs::permissions(bmc::path(root_), fs::perms::owner_all, fs::perm_options::replace, ec);
    lockDirectory();
    try {
        openStorage();
    } catch (...) {
        if (lockFd_ >= 0)
            ::close(lockFd_);
        throw;
    }
    startDriver();
}

Bot::~Bot() {
    try {
        close();
    } catch (...) {
    }
}

// One process at a time on a data directory: two would write the same mailbox and node store.
void Bot::lockDirectory() {
    const auto lock = root_ + "/bot.lock";
    lockFd_ = ::open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockFd_ < 0)
        throw Error(Code::Io, "Cannot open the lock file in the data directory");
    if (::flock(lockFd_, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = errno == EWOULDBLOCK;
        ::close(lockFd_);
        lockFd_ = -1;
        throw Error(busy ? Code::State : Code::Io,
                    busy ? "The data directory is in use by another process"
                         : "Cannot lock the data directory");
    }
}

void Bot::openStorage() {
    // The folders of the first chat_responder keep their names and open as they were.
    const bool legacy = !fs::exists(bmc::path(root_ + "/bot.bmvault")) &&
                        fs::exists(bmc::path(root_ + "/responder.bmvault"));
    vaultPath_ = root_ + (legacy ? "/responder.bmvault" : "/bot.bmvault");
    mailPath_ = root_ + (legacy ? "/responder.bmmail" : "/bot.bmmail");
    const std::string password =
        o_.secret ? bmc::hex(bmc::hash(bmc::Digest::Sha256,
                                       bmc::view("ynotbit chat responder vault\n" + o_.phrase)))
                  : std::string("ynotbit chat responder");
    auto identity = bmc::phrase::identity(o_.phrase, o_.label, 0);
    address_ = identity.address;
    vault_ = std::make_shared<bmc::Vault>();
    mailbox_ = std::make_shared<bmc::Mailbox>();
    if (!fs::exists(bmc::path(vaultPath_))) {
        vault_->create(vaultPath_, password);
        try {
            vault_->addIdentity(identity);
            const auto key = vault_->addMailboxKey();
            mailbox_->create(mailPath_, key, vault_->mailboxKey(key));
        } catch (...) {
            // Nothing but this phrase's identity was in the vault: no user data to keep.
            mailbox_->close();
            vault_->lock();
            std::error_code ignored;
            fs::remove(bmc::path(vaultPath_), ignored);
            fs::remove(bmc::path(mailPath_), ignored);
            throw;
        }
    } else {
        try {
            vault_->unlock(vaultPath_, password);
        } catch (const Error &e) {
            if (std::string(e.what()).find("Wrong password") != std::string::npos)
                throw Error(Code::Auth,
                            std::string(e.what()) +
                                (o_.secret ? " (is it the phrase this was made with?)" : ""));
            throw;
        }
        bool mine = false;
        for (const auto &i : vault_->identities())
            mine = mine || (!i.chan && i.address == address_);
        if (!mine) {
            vault_->lock();
            throw Error(Code::Auth, "This data directory was made with another phrase");
        }
        if (!fs::exists(bmc::path(mailPath_))) {
            const auto ids = vault_->mailboxIds();
            const auto key = ids.empty() ? vault_->addMailboxKey() : ids.front();
            mailbox_->create(mailPath_, key, vault_->mailboxKey(key));
            note("The mailbox was missing: made a new one");
        } else {
            bool opened = false;
            for (const auto &id : vault_->mailboxIds()) {
                try {
                    mailbox_->open(mailPath_, vault_->mailboxKey(id));
                    if (mailbox_->keyId() == id) {
                        opened = true;
                        break;
                    }
                } catch (...) {
                }
                mailbox_->close();
            }
            if (!opened) {
                vault_->lock();
                throw Error(Code::Auth,
                            "This vault cannot open that mailbox, or the mailbox is damaged");
            }
        }
    }
    client_ = std::make_unique<bmc::Client>(vault_, mailbox_, root_);
    client_->bindMailboxToCache();
}

void Bot::startDriver() {
    startedAt_ = bmc::now();
    beat_ = steadySeconds();
    if (const auto bin = findNode(o_.node.bin); !bin.empty()) {
        bmc::NodeConfig nc;
        nc.root = root_;
        nc.maildir = root_ + "/unused-maildir";
        nc.bin = bin;
        nc.listen = o_.node.listen && o_.node.proxy.empty();
        nc.retentionMB = o_.node.retentionMB;
        nc.retentionDays = o_.node.retentionDays;
        nc.clampRetention();
        nc.maxOutgoing = o_.node.maxOutgoing;
        nc.maxIncoming = o_.node.maxIncoming;
        nc.port = o_.node.port;
        nc.peer = o_.node.peer;
        nc.proxy = o_.node.proxy;
        nc.linkOnly = !o_.node.publicNetwork;
        node_ = std::make_unique<bmc::NodeSupervisor>(nc);
        watch_.lonelyAfter = int64_t(o_.node.watchdogMinutes) * 60;
        startNode(startedAt_);
    } else if (!o_.node.bin.empty()) {
        note("Node: bmnode was not found; set node.bin or BMBOT_NODE");
    }
    driver_ = std::thread([this] { drive(); });
    if (o_.hangSeconds > 0)
        hang_ = std::thread([this] { watchHang(); });
}

// --- the driver -------------------------------------------------------------------------------

void Bot::note(const std::string &text) {
    // {"kind":"note"}: a problem or a watchdog restart, said once and not kept.
    notes_.push_back("{\"kind\":\"note\",\"text\":" + quote(text) +
                     ",\"time\":" + number(bmc::now()) + "}");
    while (notes_.size() > kMaxNotes)
        notes_.pop_front();
    eventCv_.notify_all();
}

// The same problem is said again only after a minute: a full disk is not a thousand lines.
void Bot::problem(const std::string &text) {
    const auto now = bmc::now();
    if (text == lastProblem_ && now - lastProblemAt_ < 60)
        return;
    lastProblem_ = text;
    lastProblemAt_ = now;
    note("Problem: " + text);
}

void Bot::drive() {
    std::unique_lock lock(m_);
    while (!stop_) {
        beat_ = steadySeconds();
        try {
            tick();
        } catch (const std::exception &e) {
            problem(e.what());
        } catch (...) {
            problem("unknown error");
        }
        beat_ = steadySeconds();
        eventCv_.notify_all();
        // Catching up (a new identity, the first sync) goes as fast as the lock can be spared.
        const auto pause = std::chrono::milliseconds(upToDate_ ? o_.stepMs : 10);
        driverCv_.wait_for(lock, pause, [this] { return stop_ || wake_; });
        wake_ = false;
    }
}

// Session::tick, without the Qt: look at the node, then step the client.
void Bot::tick() {
    const auto now = bmc::now();
    pollNode(now);
    if (const auto read = bmc::NodeStatus::fromFile(bmc::path(root_ + "/status.json")))
        status_ = read;
    if (node_ && node_->running()) {
        // A node that runs but does no good (its status stopped, or no peer for too long) is
        // started again; one that exited is started again by pollNode().
        const auto verdict =
            watch_.judge(now, status_ ? status_->time : 0, status_ ? status_->peers : 0);
        if (verdict != bmc::NodeWatch::Verdict::Fine) {
            watchdogNote_ = verdict == bmc::NodeWatch::Verdict::Hung
                                ? "its status stopped for " + number(watch_.hungAfter) + " s"
                                : "no peer for " + number(watch_.lonelyAfter / 60) + " minutes";
            ++watchdogRestarts_;
            note("Watchdog: restarted its node, " + watchdogNote_);
            restartNode(now);
        }
    }
    const auto report = client_->step(now, node_ ? node_->running() : true);
    upToDate_ = !report.open || report.upToDate;
    working_ = report.working;
    if (report.pruned && !prunedNoted_) {
        prunedNoted_ = true;
        note("Retention cleanup removed objects this mailbox had not read; "
             "some older messages may be lost");
    }
}

// What the node said, and when it went.
void Bot::pollNode(int64_t now) {
    if (!node_)
        return;
    const auto &line = node_->standardError();
    if (!line.empty() && line != lastStderr_) {
        lastStderr_ = line;
        nodeError_ = line;
        note("Node: " + node_->standardErrorChunk());
    }
    if (node_->poll()) {
        if (nodeError_.empty() && (node_->exitCode() || node_->crashed()))
            nodeError_ = node_->crashed() ? "crashed" : "exit code " + number(node_->exitCode());
        note("The node stopped" + (nodeError_.empty() ? std::string() : ": " + nodeError_) +
             "; starting it again in " + number(kNodeRetrySeconds) + " s");
        restartAt_ = now + kNodeRetrySeconds;
    }
    if (restartAt_ && now >= restartAt_ && !node_->running()) {
        restartAt_ = 0;
        startNode(now);
    }
}

void Bot::startNode(int64_t now) {
    watch_.restarted(now);
    std::error_code ignored;
    fs::remove(bmc::path(root_ + "/status.json"), ignored);
    status_.reset();
    lastStderr_.clear();
    nodeError_.clear();
    if (!node_->start()) {
        nodeError_ = node_->lastError();
        note("Node: " + nodeError_);
        restartAt_ = now + kNodeRetrySeconds;
    }
}

void Bot::restartNode(int64_t now) {
    ++nodeRestarts_;
    node_->stop();
    startNode(now);
}

void Bot::watchHang() {
    std::unique_lock lock(hangMutex_);
    while (!hangStop_) {
        hangCv_.wait_for(lock, std::chrono::seconds(1), [this] { return hangStop_; });
        if (hangStop_)
            break;
        const auto stalled = steadySeconds() - beat_.load();
        if (stalled < o_.hangSeconds)
            continue;
        if (o_.onHang) {
            beat_ = steadySeconds();
            o_.onHang(stalled);
        } else {
            std::fprintf(stderr,
                         "bmbot: the driver loop has not run for %lld s; aborting so that it is "
                         "started again\n",
                         static_cast<long long>(stalled));
            std::abort();
        }
    }
}

// --- the API ----------------------------------------------------------------------------------

void Bot::ensureOpen() const {
    if (closing_ || closed_)
        throw Error(Code::State, "The bot is closed");
}

namespace {
std::string chatEvent(const bmc::ChatMessage &c) {
    return "{\"id\":" + quote(c.id) + ",\"kind\":\"chat\",\"to\":" + quote(c.localAddress) +
           ",\"from\":" + quote(c.peerAddress) + ",\"subject\":\"\",\"text\":" + quote(c.body) +
           ",\"seqno\":" + number(c.seqno) + ",\"received\":" + number(c.received) + "}";
}
std::string letterEvent(const bmc::Message &m) {
    return "{\"id\":" + quote(m.hash) + ",\"kind\":\"letter\",\"to\":" + quote(m.to) +
           ",\"from\":" + quote(m.from) + ",\"subject\":" + quote(m.subject) +
           ",\"text\":" + quote(m.body) + ",\"seqno\":0,\"received\":" + number(m.received) + "}";
}
} // namespace

// The unread chats are the queue, and the mailbox keeps it: oldest first, one handed out once per
// run, read only by done().
bool Bot::takeChat(std::string &out) {
    for (const auto &c : client_->unreadChats(int(handedChats_.size()) + 16)) {
        if (handedChats_.count(c.id))
            continue;
        if (handedChats_.size() >= kMaxHanded)
            throw Error(Code::State, "Too many events are waiting for done()");
        handedChats_.insert(c.id);
        out = chatEvent(c);
        return true;
    }
    return false;
}

bool Bot::takeLetter(std::string &out) {
    std::vector<std::string> hashes; // newest first
    try {
        hashes = mailbox_->messageHashes("Inbox", {}, true, false);
    } catch (const std::exception &) {
        return false;
    }
    for (auto it = hashes.rbegin(); it != hashes.rend(); ++it) {
        if (handedLetters_.count(*it))
            continue;
        if (handedLetters_.size() >= kMaxHanded)
            throw Error(Code::State, "Too many events are waiting for done()");
        const auto letter = mailbox_->message(*it);
        handedLetters_.insert(*it);
        out = letterEvent(letter);
        return true;
    }
    return false;
}

int Bot::next(int timeoutMs, std::string &out) {
    out.clear();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    std::unique_lock lock(m_);
    struct Inflight {
        Bot &bot;
        explicit Inflight(Bot &b) : bot(b) {
            ++bot.inflight_;
        }
        ~Inflight() {
            if (--bot.inflight_ == 0)
                bot.idleCv_.notify_all();
        }
    } inflight(*this);
    for (;;) {
        ensureOpen();
        if (!notes_.empty()) {
            out = std::move(notes_.front());
            notes_.pop_front();
            return 0;
        }
        if (takeChat(out) || takeLetter(out))
            return 0;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return 1;
        eventCv_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(500)));
    }
}

void Bot::rememberDone(const std::string &id) {
    if (recentDone_.insert(id).second)
        recentDoneOrder_.push_back(id);
    while (recentDoneOrder_.size() > kRememberDone) {
        recentDone_.erase(recentDoneOrder_.front());
        recentDoneOrder_.pop_front();
    }
}

void Bot::done(const std::string &id) {
    std::lock_guard lock(m_);
    ensureOpen();
    if (id.empty())
        throw Error(Code::Argument, "An event id is required");
    if (handedChats_.erase(id)) {
        client_->markChatMessageRead(id);
        rememberDone(id);
        return;
    }
    if (handedLetters_.erase(id)) {
        mailbox_->markRead(id);
        rememberDone(id);
        return;
    }
    // Not handed out in this run (a consumer that kept the id over a restart), or done already.
    if (recentDone_.count(id) || client_->markChatMessageRead(id))
        return;
    try {
        mailbox_->message(id); // throws for an id that is no letter of this mailbox
        if (mailbox_->unread(id))
            mailbox_->markRead(id);
        return;
    } catch (const std::exception &) {
    }
    throw Error(Code::Argument, "Unknown event");
}

std::string Bot::fromOrDefault(const std::string &from) const {
    return from.empty() ? address_ : from;
}

std::string Bot::sendChat(const std::string &from, const std::string &to, const std::string &text) {
    std::lock_guard lock(m_);
    ensureOpen();
    const auto id = client_->sendChat(fromOrDefault(from), trimmed(to), text, bmc::now());
    wake_ = true;
    driverCv_.notify_one();
    return id;
}

std::string Bot::sendLetter(const std::string &from, const std::string &to,
                            const std::string &subject, const std::string &body) {
    std::lock_guard lock(m_);
    ensureOpen();
    const auto id = mailbox_->saveDraft({}, fromOrDefault(from), trimmed(to), subject, body);
    try {
        mailbox_->setSetting("draftkind:" + id, "direct");
        client_->sendLetter(id, bmc::now());
    } catch (...) {
        // A letter that was refused is not left in Drafts: it is the draft this call made.
        try {
            mailbox_->moveMessage(id, "Trash");
            mailbox_->deleteMessage(id);
        } catch (...) {
        }
        throw;
    }
    wake_ = true;
    driverCv_.notify_one();
    return id;
}

std::string Bot::state(const std::string &id) {
    std::lock_guard lock(m_);
    ensureOpen();
    if (id.empty())
        return {};
    if (auto chat = client_->chatState(id); !chat.empty())
        return chat;
    try {
        return mailbox_->outgoing(id).state;
    } catch (const std::exception &) {
        return {};
    }
}

std::string Bot::networkLine(int64_t now) const {
    if (node_ && !node_->running())
        return nodeError_.empty()
                   ? "Node stopped · outgoing objects stay queued"
                   : "Node stopped: " + nodeError_ + " · outgoing objects stay queued";
    if (!status_)
        return "Node starting · connecting to peers";
    if (now - status_->time > 10)
        return "Node status unavailable";
    auto line = status_->peers ? "Connected peers: " + std::to_string(status_->peers) +
                                     " · receiving and relaying"
                               : std::string("No connected peers · waiting for network");
    if (status_->pending > 0)
        line += " · objects downloading: " + std::to_string(status_->pending);
    const int wanted = o_.node.port > 0 && o_.node.port < 65536 ? o_.node.port : bmc::kNodePort;
    if (status_->listenPort > 0 && status_->listenPort != wanted)
        line += " · port " + std::to_string(wanted) + " is taken, listening on " +
                std::to_string(status_->listenPort);
    return line;
}

std::string Bot::status() {
    std::lock_guard lock(m_);
    ensureOpen();
    const auto now = bmc::now();
    const bool managed = node_ != nullptr;
    const auto &s = status_;
    std::string out = "{\"time\":" + number(now) + ",\"uptime\":" + number(now - startedAt_) +
                      ",\"address\":" + quote(address_) + ",\"network\":" + quote(networkLine(now));
    out += ",\"working\":" + std::string(working_ ? "true" : "false");
    out += ",\"up_to_date\":" + std::string(upToDate_ ? "true" : "false");
    out += ",\"node\":{\"managed\":" + std::string(managed ? "true" : "false");
    out += ",\"running\":" + std::string(managed ? (node_->running() ? "true" : "false")
                                                 : (s && now - s->time <= 10 ? "true" : "false"));
    out += ",\"pid\":" + number(managed ? node_->pid() : 0);
    out += ",\"peers\":" + number(s ? s->peers : 0);
    out += ",\"outgoing\":" + number(s ? s->establishedOutgoing : 0);
    out += ",\"incoming\":" + number(s ? s->establishedIncoming : 0);
    out += ",\"objects\":" + number(s ? s->objects : 0);
    out += ",\"pending\":" + number(s ? s->pending : 0);
    out += ",\"listen_port\":" + number(s ? s->listenPort : 0);
    out += ",\"status_age\":" + number(s ? now - s->time : -1);
    out += ",\"restarts\":" + number(nodeRestarts_);
    out += ",\"watchdog_restarts\":" + number(watchdogRestarts_) + "}}";
    return out;
}

void Bot::inspect(const std::function<void(bmc::Client &, bmc::Vault &)> &f) {
    std::lock_guard lock(m_);
    ensureOpen();
    f(*client_, *vault_);
}

void Bot::close() {
    {
        std::unique_lock lock(m_);
        if (closed_ || closing_) {
            // Another thread is closing it: wait until it is done.
            idleCv_.wait(lock, [this] { return closed_; });
            return;
        }
        closing_ = true;
    }
    eventCv_.notify_all();
    {
        std::lock_guard lock(hangMutex_);
        hangStop_ = true;
    }
    hangCv_.notify_all();
    if (hang_.joinable())
        hang_.join();
    {
        std::lock_guard lock(m_);
        stop_ = true;
    }
    driverCv_.notify_all();
    if (driver_.joinable())
        driver_.join();
    std::unique_lock lock(m_);
    idleCv_.wait(lock, [this] { return inflight_ == 0; });
    if (client_)
        client_->delivery().stop();
    if (node_)
        node_->stop();
    if (mailbox_)
        mailbox_->close();
    if (vault_)
        vault_->lock();
    if (lockFd_ >= 0) {
        ::close(lockFd_);
        lockFd_ = -1;
    }
    closed_ = true;
    idleCv_.notify_all();
}

} // namespace bmbot_lib
