// Importing a PyBitmessage data folder into the open vault and mailbox: what the folder holds (for
// the dialog to show first), then the import itself, a slice of work at a time so that a window can
// show its progress, and a report of what became of everything.
#pragma once
#include <bmc/mailbox.h>
#include <bmc/pybitmessage.h>
#include <bmc/vault.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace bmc::pybm {

// Why a folder cannot be imported from.
enum class Trouble { None, NothingThere, Unreadable, Busy, Interrupted, Failed };

// What a folder holds, counted without reading a letter.
struct Preview {
    std::string folder;
    bool hasKeys = false, hasMessages = false;
    bool keysEncrypted = false; // keys.dat says its keys are encrypted: no identity can be read
    bool keysUnreadable =
        false; // keys.dat is there and cannot be read: no identity, the letters go on
    Trouble trouble = Trouble::None;
    std::string detail; // the reason in English, for a log or a details line
    int version = 0;
    // keys.dat
    int64_t identities = 0, chans = 0, disabled = 0, mailingLists = 0, unusable = 0;
    // messages.dat: letters by what they become here
    int64_t contacts = 0, subscriptions = 0, subscriptionsDisabled = 0;
    int64_t inbox = 0, inboxUnread = 0, channelLetters = 0, channelUnread = 0;
    int64_t broadcastLetters = 0, broadcastUnread = 0, trashed = 0;
    int64_t sent = 0, unsent = 0, sentTrashed = 0;
    int64_t unlisted = 0;                     // rows in a folder PyBitmessage itself never lists
    int64_t blacklisted = 0, whitelisted = 0; // not imported
    bool running = false;                     // PyBitmessage looks to be running
    int64_t letters() const {                 // the rows that will be read
        return inbox + channelLetters + broadcastLetters + trashed + sent + unsent + sentTrashed;
    }
};
// Never throws: a folder that cannot be read says why in `trouble`. The chans the vault already
// holds count as chans (their letters are channel letters even if keys.dat is gone).
Preview preview(const std::string &folder, const Vault *vault = nullptr);

struct Options {
    std::string folder;
    bool identities = true, contacts = true, subscriptions = true;
    bool inbox = true, sent = true, trash = true;
    int chunk = 250;        // rows read and stored together
    int patienceMs = 60000; // how long to wait for a PyBitmessage that has its data locked
};

struct Progress {
    enum class Phase { Starting, Contacts, Subscriptions, Checking, Inbox, Sent, Identities, Done };
    Phase phase = Phase::Starting;
    int64_t done = 0, total = 0; // letters read so far, of those there are
    bool waiting = false;        // PyBitmessage has its data locked for the moment
};

struct Count {
    int64_t added = 0, present = 0, skipped = 0;
};
struct Report {
    Count contacts, subscriptions;
    // Letters by the folder they went to.
    Count inbox, channels, broadcasts, sent, drafts, trash;
    // The chat messages of this app that PyBitmessage's inbox holds as letters, by what became of
    // them: stored in their conversations, found there already, or (skipped) deleted over there,
    // which chats have no folder for.
    Count chats;
    int64_t letterOfUnknownFolder = 0; // rows in a folder PyBitmessage itself does not list
    int64_t withoutDate = 0;           // letters that came with no usable date (dated 1970)
    int64_t left = 0;                  // rows not read because of the options
    std::vector<IdentityImport> identities;
    bool keysEncrypted = false;
    bool keysUnreadable = false; // keys.dat is there and could not be read: no identity came in
    bool cancelled = false;
    bool finished = false; // reached the end
    Trouble trouble = Trouble::None;
    std::string detail;
    int64_t lettersAdded() const {
        return inbox.added + channels.added + broadcasts.added + sent.added + drafts.added +
               trash.added;
    }
};

class Importer {
    struct State;
    std::unique_ptr<State> s_;

  public:
    Importer(Options options, Mailbox &mailbox, Vault &vault);
    ~Importer();
    Importer(const Importer &) = delete;
    Importer &operator=(const Importer &) = delete;
    // About `budgetMs` of work (at least one chunk). False once there is no more: done, cancelled
    // or stopped by trouble (see report()).
    bool step(int budgetMs);
    // Ends the import at the next step. What is stored stays; running it again carries on.
    void cancel();
    const Progress &progress() const;
    const Report &report() const;
};

// How long a piece of work has left, from how fast it went so far. Times are seconds from any
// fixed start. Not an answer at first: a few seconds of work and some share of it must be behind.
class Estimator {
    double last_ = 0, lastDone_ = 0, first_ = -1, rate_ = 0;
    double total_ = 0, done_ = 0;
    bool haveRate_ = false;

  public:
    void sample(double seconds, double done, double total);
    // Seconds left; negative while it is too early to say.
    double secondsLeft() const;
};
} // namespace bmc::pybm
