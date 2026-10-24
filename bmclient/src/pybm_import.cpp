#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/error.h>
#include <bmc/hash.h>
#include <bmc/path.h>
#include <bmc/pybm_import.h>
#include <bmc/text.h>
#include <filesystem>
#include <map>

namespace bmc::pybm {
namespace {
enum class Kind { Direct, Channel, Broadcast };

Kind kindOf(const std::string &to, const std::set<std::string> &chans) {
    if (to == kBroadcastAddress)
        return Kind::Broadcast;
    return chans.count(to) ? Kind::Channel : Kind::Direct;
}

bool exists(const std::string &file) {
    std::error_code ignored;
    return std::filesystem::exists(path(file), ignored);
}

// The addresses of the chans the keys file and the vault hold.
std::set<std::string> chansOf(const KeysFile &keys, const Vault *vault) {
    std::set<std::string> chans;
    for (const auto &e : keys.entries)
        if (e.chan)
            chans.insert(e.address);
    if (vault)
        for (const auto &i : vault->identities())
            if (i.chan)
                chans.insert(i.address);
    return chans;
}

// An id of our own for a letter that carries none: what it says, and when.
std::string idFromText(std::string_view from, std::string_view to, std::string_view subject,
                       std::string_view body, int64_t time) {
    const auto digits =
        hex(view(letterFingerprint(from, to, std::string(subject) + std::to_string(time), body)));
    return "pybm-" + digits.substr(0, 32);
}
} // namespace

Preview preview(const std::string &folder, const Vault *vault) {
    Preview p;
    p.folder = folder;
    p.hasKeys = exists(folder + "/keys.dat");
    p.hasMessages = exists(folder + "/messages.dat");
    p.running = exists(folder + "/singleton.lock");
    if (!p.hasKeys && !p.hasMessages) {
        p.trouble = Trouble::NothingThere;
        p.detail = "There is no keys.dat or messages.dat in this folder";
        return p;
    }
    KeysFile keys;
    if (p.hasKeys) {
        try {
            keys = readKeys(folder + "/keys.dat");
        } catch (const std::exception &e) {
            p.keysUnreadable = true;
            p.detail = e.what();
        }
        p.keysEncrypted = keys.encrypted;
        if (!keys.encrypted)
            for (const auto &e : keys.entries) {
                ++p.identities;
                p.chans += e.chan;
                p.disabled += !e.enabled;
                p.mailingLists += e.mailingList;
                p.unusable += e.problem != KeyProblem::None;
            }
    }
    if (!p.hasMessages)
        return p;
    const auto chans = chansOf(keys.encrypted ? KeysFile() : std::move(keys), vault);
    try {
        MessagesFile file(folder + "/messages.dat", 250);
        const auto census = file.census();
        p.version = census.version;
        for (const auto &g : census.inbox) {
            if (g.folder == "inbox") {
                switch (kindOf(g.to, chans)) {
                case Kind::Direct:
                    p.inbox += g.count;
                    p.inboxUnread += g.read ? 0 : g.count;
                    break;
                case Kind::Channel:
                    p.channelLetters += g.count;
                    p.channelUnread += g.read ? 0 : g.count;
                    break;
                case Kind::Broadcast:
                    p.broadcastLetters += g.count;
                    p.broadcastUnread += g.read ? 0 : g.count;
                    break;
                }
            } else if (g.folder == "trash") {
                p.trashed += g.count;
            } else {
                p.unlisted += g.count;
            }
        }
        for (const auto &g : census.sent) {
            if (g.folder == "sent")
                (sentState(g.status) == SentState::Done ? p.sent : p.unsent) += g.count;
            else if (g.folder == "trash")
                p.sentTrashed += g.count;
            else
                p.unlisted += g.count;
        }
        p.contacts = census.contacts;
        p.subscriptions = census.subscriptions;
        p.subscriptionsDisabled = census.subscriptionsDisabled;
        p.blacklisted = census.blacklisted;
        p.whitelisted = census.whitelisted;
    } catch (const Busy &e) {
        p.trouble = Trouble::Busy;
        p.detail = e.what();
    } catch (const Interrupted &e) {
        p.trouble = Trouble::Interrupted;
        p.detail = e.what();
    } catch (const std::exception &e) {
        p.trouble = Trouble::Unreadable;
        p.detail = e.what();
    }
    return p;
}

struct Importer::State {
    Options options;
    Mailbox &mailbox;
    Vault &vault;
    Progress progress;
    Report report;
    bool cancelRequested = false, stopped = false;
    // Gathered when the import starts.
    bool begun = false;
    std::unique_ptr<MessagesFile> file;
    KeysFile keys;
    std::set<std::string> chans;
    std::unordered_set<std::string> known; // letters of this mailbox, by what they say
    int64_t inboxAfter = 0, sentAfter = 0;
    bool waiting = false;
    std::chrono::steady_clock::time_point waitingSince;

    State(Options o, Mailbox &m, Vault &v) : options(std::move(o)), mailbox(m), vault(v) {}

    void stop(Trouble trouble, const std::string &detail) {
        report.trouble = trouble;
        report.detail = detail;
        stopped = true;
    }
    using Phase = Progress::Phase;
    // The next phase after `from` that the options and the folder leave to do.
    Phase after(Phase from) const {
        const bool messages = file != nullptr;
        for (int next = int(from) + 1; next <= int(Phase::Done); ++next) {
            switch (Phase(next)) {
            case Phase::Contacts:
                if (messages && options.contacts)
                    return Phase::Contacts;
                break;
            case Phase::Subscriptions:
                if (messages && options.subscriptions)
                    return Phase::Subscriptions;
                break;
            case Phase::Checking:
            case Phase::Inbox:
                if (messages && (options.inbox || options.trash))
                    return Phase(next);
                break;
            case Phase::Sent:
                if (messages && (options.sent || options.trash))
                    return Phase::Sent;
                break;
            case Phase::Identities:
                if (options.identities && !keys.entries.empty() && !keys.encrypted)
                    return Phase::Identities;
                break;
            default:
                return Phase::Done;
            }
        }
        return Phase::Done;
    }

    void begin() {
        const auto keysFile = options.folder + "/keys.dat",
                   messagesFile = options.folder + "/messages.dat";
        const bool hasKeys = exists(keysFile), hasMessages = exists(messagesFile);
        if (!hasKeys && !hasMessages) {
            stop(Trouble::NothingThere, "There is no keys.dat or messages.dat in this folder");
            return;
        }
        // Opening can find PyBitmessage busy; nothing is changed before it has opened.
        std::unique_ptr<MessagesFile> opened;
        Census census;
        if (hasMessages) {
            opened = std::make_unique<MessagesFile>(messagesFile, 250);
            census = opened->census();
        }
        KeysFile read;
        if (hasKeys) {
            try {
                read = readKeys(keysFile);
            } catch (const std::exception &) {
                read = KeysFile(); // not readable: no identities, the letters go on
                report.keysUnreadable = true;
            }
        }
        file = std::move(opened);
        keys = std::move(read);
        report.keysEncrypted = keys.encrypted;
        chans = chansOf(keys, &vault);
        int64_t total = 0, inboxRows = 0, sentRows = 0;
        for (const auto &g : census.inbox)
            inboxRows += g.count;
        for (const auto &g : census.sent)
            sentRows += g.count;
        // The deleted letters are rows of the same two tables, so asking for them reads those.
        if (options.inbox || options.trash)
            total += inboxRows;
        else
            report.left += inboxRows;
        if (options.sent || options.trash)
            total += sentRows;
        else
            report.left += sentRows;
        progress.total = total;
        begun = true;
        progress.phase = after(Phase::Starting);
    }

    void contacts() {
        std::vector<Contact> named;
        int64_t skipped = 0;
        for (const auto &c : file->contacts()) {
            if (c.address.empty() || text::isBlank(c.label))
                ++skipped;
            else
                named.push_back({c.address, c.label, 0});
        }
        const int added = mailbox.importContacts(named);
        report.contacts.added = added;
        report.contacts.present = int64_t(named.size()) - added;
        report.contacts.skipped = skipped;
        progress.phase = after(Phase::Contacts);
    }

    void subscriptions() {
        std::vector<Subscription> rows;
        int64_t skipped = 0;
        for (const auto &s : file->subscriptions()) {
            if (s.address.empty())
                ++skipped;
            else
                rows.push_back({s.address, s.label});
        }
        const int added = mailbox.importSubscriptions(rows);
        report.subscriptions.added = added;
        report.subscriptions.present = int64_t(rows.size()) - added;
        report.subscriptions.skipped = skipped;
        progress.phase = after(Phase::Subscriptions);
    }

    void checking() {
        known = mailbox.letterFingerprints({"Inbox", "Trash", "Archive"});
        progress.phase = after(Phase::Checking);
    }

    Count &counter(const std::string &folder) {
        if (folder == "Inbox")
            return report.inbox;
        if (folder == "Channels")
            return report.channels;
        if (folder == "Broadcasts")
            return report.broadcasts;
        if (folder == "Sent")
            return report.sent;
        if (folder == "Drafts")
            return report.drafts;
        return report.trash;
    }

    // The letters of one chunk, by the folder they go to, and its chat messages, stored in one
    // commit.
    void store(std::map<std::string, std::vector<ImportedLetter>> &byFolder,
               const std::vector<ImportedChat> &chats) {
        Mailbox::Batch batch(mailbox);
        for (auto &[folder, letters] : byFolder) {
            const auto counts = mailbox.importLetters(letters);
            counter(folder).added += counts.added;
            counter(folder).present += counts.present;
        }
        const auto counts = mailbox.importChats(chats);
        report.chats.added += counts.added;
        report.chats.present += counts.present;
        batch.commit();
    }

    void inbox() {
        const auto rows = file->inbox(inboxAfter, std::max(1, options.chunk));
        if (rows.empty()) {
            progress.phase = after(Phase::Inbox);
            return;
        }
        std::map<std::string, std::vector<ImportedLetter>> byFolder;
        std::vector<ImportedChat> chats;
        for (const auto &row : rows) {
            const bool trashed = row.folder == "trash";
            if (!trashed && row.folder != "inbox") {
                ++report.letterOfUnknownFolder;
                continue;
            }
            if (trashed ? !options.trash : !options.inbox) {
                ++report.left;
                continue;
            }
            const auto kind = kindOf(row.to, chans);
            const std::string natural = kind == Kind::Direct    ? "Inbox"
                                        : kind == Kind::Channel ? "Channels"
                                                                : "Broadcasts";
            const int64_t received = row.receivedKnown ? row.received : 0;
            const auto id = row.msgid.size() == 32
                                ? hex(row.msgid)
                                : idFromText(row.from, row.to, row.subject, row.body, received);
            // A chat message of this app, which the app that PyBitmessage shares the identity with
            // sent: it is told as it is when it arrives (a direct letter, no subject, the app's
            // JSON as the text; the reader has cleaned the text of bytes that are not UTF-8), and
            // belongs to the chats, not to the inbox.
            ChatPayload payload;
            if (kind == Kind::Direct && !row.from.empty() &&
                parseChatMessage(row.subject, row.body, &payload) == ChatParseResult::Chat) {
                if (trashed) { // chats have no trash to be deleted into
                    ++report.chats.skipped;
                    continue;
                }
                if (!row.receivedKnown)
                    ++report.withoutDate;
                ImportedChat chat;
                chat.id = id;
                chat.local = row.to;
                chat.peer = row.from;
                chat.seqno = payload.seqno;
                chat.body = std::move(payload.body);
                chat.received = received;
                chat.unread = !row.read;
                chats.push_back(std::move(chat));
                continue;
            }
            ImportedLetter l;
            l.sender = row.from;
            l.recipient = kind == Kind::Broadcast ? row.from : row.to;
            l.subject = row.subject;
            l.body = row.body;
            l.folder = trashed ? "Trash" : natural;
            l.previousFolder = natural;
            l.unread = !row.read;
            l.received = received;
            if (!row.receivedKnown)
                ++report.withoutDate;
            l.id = id;
            if (row.msgid.size() == 32)
                l.objectHash = id;
            // A letter with an acknowledgment is kept here under an id made of what it says, so the
            // object hash cannot find a copy the scan made; its text can.
            if (kind == Kind::Direct &&
                known.count(letterFingerprint(l.sender, l.recipient, l.subject, l.body))) {
                ++counter(l.folder).present;
                continue;
            }
            byFolder[l.folder].push_back(std::move(l));
        }
        store(byFolder, chats);
        progress.done += int64_t(rows.size());
        inboxAfter = rows.back().rowid;
    }

    void sent() {
        const auto rows = file->sent(sentAfter, std::max(1, options.chunk));
        if (rows.empty()) {
            progress.phase = after(Phase::Sent);
            return;
        }
        std::map<std::string, std::vector<ImportedLetter>> byFolder;
        for (const auto &row : rows) {
            const bool trashed = row.folder == "trash";
            if (!trashed && row.folder != "sent") {
                ++report.letterOfUnknownFolder;
                continue;
            }
            if (trashed ? !options.trash : !options.sent) {
                ++report.left;
                continue;
            }
            // Only what went out is Sent; the rest was never delivered, and stays a draft.
            const std::string natural =
                sentState(row.status) == SentState::Done ? "Sent" : "Drafts";
            const bool broadcast = row.to == kBroadcastAddress;
            ImportedLetter l;
            l.sender = row.from;
            l.recipient = broadcast ? std::string() : row.to;
            l.subject = row.subject;
            l.body = row.body;
            l.folder = trashed ? "Trash" : natural;
            l.previousFolder = natural;
            l.broadcast = broadcast;
            l.received = row.sentKnown ? row.sent : 0;
            if (!row.sentKnown)
                ++report.withoutDate;
            l.id = !row.ackdata.empty()
                       ? "pybm-" + hex(row.ackdata)
                       : idFromText(row.from, row.to, row.subject, row.body, l.received);
            byFolder[l.folder].push_back(std::move(l));
        }
        store(byFolder, {});
        progress.done += int64_t(rows.size());
        sentAfter = rows.back().rowid;
    }

    void identities() {
        report.identities = vault.importEntries(keys.entries);
        const bool added = std::any_of(
            report.identities.begin(), report.identities.end(),
            [](const IdentityImport &i) { return i.result == IdentityImport::Result::Added; });
        // The identities that came in are looked for in everything the node still holds.
        if (added && mailbox.isOpen())
            mailbox.advance(0);
        progress.phase = after(Phase::Identities);
    }

    void unit() {
        if (!begun) {
            begin();
            return;
        }
        switch (progress.phase) {
        case Phase::Starting:
            progress.phase = after(Phase::Starting);
            break;
        case Phase::Contacts:
            contacts();
            break;
        case Phase::Subscriptions:
            subscriptions();
            break;
        case Phase::Checking:
            checking();
            break;
        case Phase::Inbox:
            inbox();
            break;
        case Phase::Sent:
            sent();
            break;
        case Phase::Identities:
            identities();
            break;
        case Phase::Done:
            break;
        }
        if (progress.phase == Phase::Done) {
            report.finished = true;
            stopped = true;
        }
    }
};

Importer::Importer(Options options, Mailbox &mailbox, Vault &vault)
    : s_(std::make_unique<State>(std::move(options), mailbox, vault)) {}
Importer::~Importer() = default;

bool Importer::step(int budgetMs) {
    auto &s = *s_;
    if (s.stopped)
        return false;
    if (s.cancelRequested) {
        s.report.cancelled = true;
        s.stopped = true;
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    do {
        try {
            s.unit();
            s.waiting = s.progress.waiting = false;
        } catch (const Busy &e) {
            // PyBitmessage is writing: try again at the next step, for a while.
            const auto now = std::chrono::steady_clock::now();
            if (!s.waiting) {
                s.waiting = true;
                s.waitingSince = now;
            }
            s.progress.waiting = true;
            if (now - s.waitingSince > std::chrono::milliseconds(s.options.patienceMs))
                s.stop(Trouble::Busy, e.what());
            return !s.stopped;
        } catch (const Interrupted &e) {
            s.stop(Trouble::Interrupted, e.what());
        } catch (const std::exception &e) {
            // The mailbox or the vault could not be written, or the file went bad half way.
            s.stop(s.begun ? Trouble::Failed : Trouble::Unreadable, e.what());
        }
        if (s.stopped)
            return false;
    } while (std::chrono::steady_clock::now() - start <
             std::chrono::milliseconds(std::max(0, budgetMs)));
    return true;
}

void Importer::cancel() {
    s_->cancelRequested = true;
}
const Progress &Importer::progress() const {
    return s_->progress;
}
const Report &Importer::report() const {
    return s_->report;
}

void Estimator::sample(double seconds, double done, double total) {
    total_ = total;
    if (first_ < 0) {
        first_ = last_ = seconds;
        lastDone_ = done_ = done;
        return;
    }
    if (seconds <= last_) // a clock that does not move forward says nothing
        return;
    // How fast it went since the last sample, smoothed so that one slow chunk does not swing it.
    const double instant = (done - lastDone_) / (seconds - last_);
    if (instant > 0) {
        rate_ = haveRate_ ? 0.3 * instant + 0.7 * rate_ : instant;
        haveRate_ = true;
    }
    last_ = seconds;
    lastDone_ = done_ = done;
}

double Estimator::secondsLeft() const {
    if (total_ <= 0 || first_ < 0)
        return -1;
    if (done_ >= total_)
        return 0;
    if (!haveRate_ || rate_ <= 0 || last_ - first_ < 2.0 || done_ < 0.02 * total_)
        return -1;
    return (total_ - done_) / rate_;
}
} // namespace bmc::pybm
