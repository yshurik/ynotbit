// Reading the data folder of PyBitmessage: where it is, the identities in its keys.dat, and the
// letters, contacts and subscriptions in its messages.dat. Nothing here writes to those files, and
// nothing holds the database between two calls, so a PyBitmessage that is still running is not
// kept waiting.
#pragma once
#include <bmc/bytes.h>
#include <bmc/error.h>
#include <bmc/secret.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace bmc::pybm {

// The facts about the system that decide where PyBitmessage keeps its data.
struct Environment {
    enum class Platform { Mac, Windows, Linux };
    Platform platform = Platform::Linux;
    std::string home, appData, xdgConfigHome, bitmessageHome;
    // This process, on this system.
    static Environment current();
};
// Where PyBitmessage may keep its data, most likely first (src/paths.py): BITMESSAGE_HOME when it
// is set, then the platform's own folder, then the folder older versions used on Linux. No file is
// looked at.
std::vector<std::string> candidateFolders(const Environment &env);
// Those of them that hold a keys.dat or a messages.dat.
std::vector<std::string> findFolders(const Environment &env);

// PyBitmessage has its data locked at this moment: try again in a moment.
class Busy : public Error {
  public:
    explicit Busy(const std::string &what) : Error(Code::State, what) {}
};
// PyBitmessage was killed in the middle of a write and left its journal; a look does not repair it.
class Interrupted : public Error {
  public:
    explicit Interrupted(const std::string &what) : Error(Code::State, what) {}
};

// A private key as keys.dat writes it (wallet import format). Throws on a broken encoding or sum.
Secret decodeWif(std::string_view text);

// Why the keys of an entry cannot be used.
enum class KeyProblem { None, MissingKey, BadKey };
// One identity or chan of keys.dat.
struct KeyEntry {
    std::string address; // as the section is named, "BM-" in front
    std::string label;   // as PyBitmessage shows it ("%%" read back as "%")
    bool chan = false, enabled = true, mailingList = false;
    Secret signing{32}, encryption{32};
    KeyProblem problem = KeyProblem::None;
};
struct KeysFile {
    std::vector<KeyEntry> entries;
    bool encrypted = false; // keysencrypted = true: the keys cannot be read here
};
// Throws Error(Io) when the file cannot be read.
KeysFile readKeys(const std::string &path);

// The rows of messages.dat that matter, as stored.
struct InboxRow {
    int64_t rowid = 0;
    Bytes msgid, sighash;                // the object's inventory hash; the signature's hash
    std::string to, from, subject, body; // to: "[Broadcast subscribers]" for a broadcast
    std::string folder;                  // as stored: "inbox" or "trash"
    int64_t received = 0;                // seconds; 0 when the column holds no number
    bool receivedKnown = true;
    bool read = true;
};
struct SentRow {
    int64_t rowid = 0;
    Bytes msgid, ackdata;                // random until sent, then the inventory hash; unique
    std::string to, from, subject, body; // to: "[Broadcast subscribers]" for a broadcast
    std::string status, folder; // "msgsent", "awaitingpubkey", ...; as stored: "sent" or "trash"
    int64_t sent = 0;
    bool sentKnown = true;
};
struct SubscriptionRow {
    std::string address, label;
    bool enabled = true;
};
struct ContactRow {
    std::string address, label;
};
// How many inbox rows there are of one kind, for a count before anything is read.
struct InboxGroup {
    std::string to, folder;
    bool read = true;
    int64_t count = 0;
};
struct SentGroup {
    std::string status, folder;
    int64_t count = 0;
};
struct Census {
    int version = 0; // messages.dat's own schema version
    std::vector<InboxGroup> inbox;
    std::vector<SentGroup> sent;
    int64_t subscriptions = 0, subscriptionsDisabled = 0, contacts = 0;
    int64_t blacklisted = 0, whitelisted = 0; // not imported; counted to say so
};

// The statuses of a sent letter that tell where it is.
enum class SentState { Done, InFlight, Failed };
SentState sentState(std::string_view status);
// The address a broadcast is "sent to" and "received at" in both tables.
constexpr const char *kBroadcastAddress = "[Broadcast subscribers]";

// messages.dat, opened read-only. Every call is its own short read; between calls the file is free.
class MessagesFile {
    sqlite3 *db_ = nullptr;
    // Which optional columns this file's version has.
    bool inboxRead_ = false, inboxSighash_ = false, sentFolder_ = false, sentMsgid_ = false;

  public:
    // Throws Error(Io) with a plain sentence when the file is missing, is no database, or lacks
    // what a PyBitmessage database has; Error(State) when PyBitmessage has it locked for longer
    // than `busyMilliseconds`, or was killed in the middle of a write (its journal is then left
    // alone: a read-only look does not repair what it finds).
    explicit MessagesFile(const std::string &path, int busyMilliseconds = 5000);
    ~MessagesFile();
    MessagesFile(const MessagesFile &) = delete;
    MessagesFile &operator=(const MessagesFile &) = delete;

    Census census() const;
    std::vector<ContactRow> contacts() const;
    std::vector<SubscriptionRow> subscriptions() const;
    // The next rows after `after` (0 for the first), in rowid order, at most `limit`.
    std::vector<InboxRow> inbox(int64_t after, int limit) const;
    std::vector<SentRow> sent(int64_t after, int limit) const;
};
} // namespace bmc::pybm
