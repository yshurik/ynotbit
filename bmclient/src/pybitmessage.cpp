#include <algorithm>
#include <bmc/error.h>
#include <bmc/path.h>
#include <bmc/pybitmessage.h>
#include <bmc/sqlite.h>
#include <bmc/text.h>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <set>
#include <sodium.h>
#include <tuple>
#include <utility>
extern "C" {
#include "config.h"
#include "ntb-base58.h"
}

namespace bmc::pybm {
namespace {
std::string variable(const char *name) {
#ifdef _WIN32
    // The narrow environment is in the ANSI code page: a user name outside it would be lost.
    std::wstring wide;
    for (const char *c = name; *c; ++c)
        wide.push_back(wchar_t(*c));
    const wchar_t *value = _wgetenv(wide.c_str());
    if (!value)
        return {};
    return text::utf8FromUtf16(std::u16string_view(reinterpret_cast<const char16_t *>(value)));
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}

std::string withoutClosingSeparator(std::string folder) {
    while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\'))
        folder.pop_back();
    return folder;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto &c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
}

// Python's ConfigParser.getboolean(), with a fallback for what it would refuse.
bool truthy(std::string_view value, bool fallback) {
    const auto s = lower(text::trim(value));
    if (s == "1" || s == "true" || s == "yes" || s == "on")
        return true;
    if (s == "0" || s == "false" || s == "no" || s == "off")
        return false;
    return fallback;
}

// A time as PyBitmessage keeps it: whole seconds, in a column that may hold them as text or with a
// fraction. False when it holds no number.
bool seconds(std::string_view value, int64_t &out) {
    value = text::trim(value);
    if (value.empty())
        return false;
    int64_t whole = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), whole);
    if (error != std::errc())
        return false;
    std::string_view rest(end, size_t(value.data() + value.size() - end));
    if (!rest.empty()) {
        if (rest.front() != '.')
            return false;
        for (const char c : rest.substr(1))
            if (c < '0' || c > '9')
                return false;
    }
    out = whole;
    return true;
}

// A secret held as text for one call, wiped afterwards.
struct WipedText {
    std::string value;
    ~WipedText() {
        if (!value.empty())
            sodium_memzero(value.data(), value.size());
    }
};

// What a failed read of PyBitmessage's file comes to in a sentence the dialog can show.
[[noreturn]] void refused(sqlite3 *db, const Error &e) {
    const int code = sqlite3_extended_errcode(db);
    if (code == SQLITE_READONLY_ROLLBACK)
        throw Interrupted("PyBitmessage did not close its data properly; start it once, close it "
                          "and try again");
    if ((code & 0xff) == SQLITE_BUSY || (code & 0xff) == SQLITE_LOCKED)
        throw Busy("PyBitmessage is using its data at this moment; try again in a moment");
    throw Error(Code::Io,
                std::string("The data of PyBitmessage cannot be read (") + e.what() + ")");
}
template <class F> auto guarded(sqlite3 *db, F &&read) {
    try {
        return read();
    } catch (const Error &e) {
        refused(db, e);
    }
}

bool tableExists(sqlite3 *db, const char *table) {
    Statement s(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    s.text(1, table);
    return s.row();
}

std::set<std::string> columnsOf(sqlite3 *db, const char *table) {
    Statement s(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
    std::set<std::string> names;
    while (s.row())
        names.insert(lower(s.text(1)));
    return names;
}

// A boolean column as PyBitmessage's inserts leave it: 0 or 1, or empty in an old file (then true).
bool flag(const Statement &s, int n) {
    switch (sqlite3_column_type(s.get(), n)) {
    case SQLITE_INTEGER:
    case SQLITE_FLOAT:
        return sqlite3_column_int64(s.get(), n) != 0;
    case SQLITE_NULL:
        return true;
    default:
        return truthy(s.text(n), true);
    }
}
} // namespace

Environment Environment::current() {
    Environment env;
#if defined(__APPLE__)
    env.platform = Platform::Mac;
#elif defined(_WIN32)
    env.platform = Platform::Windows;
#else
    env.platform = Platform::Linux;
#endif
    env.home = variable("HOME");
    env.appData = variable("APPDATA");
    env.xdgConfigHome = variable("XDG_CONFIG_HOME");
    env.bitmessageHome = variable("BITMESSAGE_HOME");
    return env;
}

std::vector<std::string> candidateFolders(const Environment &env) {
    std::vector<std::string> folders;
    const auto add = [&](std::string folder) {
        if (!folder.empty() && std::find(folders.begin(), folders.end(), folder) == folders.end())
            folders.push_back(std::move(folder));
    };
    if (!env.bitmessageHome.empty())
        add(withoutClosingSeparator(env.bitmessageHome));
    switch (env.platform) {
    case Environment::Platform::Mac:
        if (!env.home.empty())
            add(withoutClosingSeparator(env.home) + "/Library/Application Support/PyBitmessage");
        break;
    case Environment::Platform::Windows:
        if (!env.appData.empty())
            add(withoutClosingSeparator(env.appData) + "\\PyBitmessage");
        break;
    case Environment::Platform::Linux:
        if (!env.xdgConfigHome.empty())
            add(withoutClosingSeparator(env.xdgConfigHome) + "/PyBitmessage");
        else if (!env.home.empty())
            add(withoutClosingSeparator(env.home) + "/.config/PyBitmessage");
        if (!env.home.empty())
            add(withoutClosingSeparator(env.home) + "/.PyBitmessage");
        break;
    }
    return folders;
}

std::vector<std::string> findFolders(const Environment &env) {
    std::vector<std::string> found;
    for (const auto &folder : candidateFolders(env)) {
        std::error_code ignored;
        if (std::filesystem::exists(path(folder + "/keys.dat"), ignored) ||
            std::filesystem::exists(path(folder + "/messages.dat"), ignored))
            found.push_back(folder);
    }
    return found;
}

Secret decodeWif(std::string_view text) {
    require(text.size() <= 128, "Invalid private key encoding");
    Secret decoded(128);
    const auto n = ntb_base58_decode(text.data(), text.size(), decoded.data(), decoded.size());
    require(n == 37 && decoded.data()[0] == 128, "Invalid private key encoding");
    unsigned char first[32], second[32];
    SHA256(decoded.data(), 33, first);
    SHA256(first, 32, second);
    const bool sumOk = CRYPTO_memcmp(decoded.data() + 33, second, 4) == 0;
    OPENSSL_cleanse(first, sizeof first);
    OPENSSL_cleanse(second, sizeof second);
    require(sumOk, "Private key checksum mismatch");
    Secret key(32);
    std::memcpy(key.data(), decoded.data() + 1, 32);
    return key;
}

KeysFile readKeys(const std::string &filePath) {
    std::FILE *f = openFile(path(filePath), "rb");
    if (!f)
        throw Error(Code::Io, "Cannot read keys.dat");
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 8 * 1024 * 1024) {
        std::fclose(f);
        throw Error(Code::Io, "Cannot read keys.dat");
    }
    WipedText content;
    content.value.resize(size_t(size));
    const bool read =
        content.value.empty() ||
        std::fread(content.value.data(), 1, content.value.size(), f) == content.value.size();
    std::fclose(f);
    if (!read)
        throw Error(Code::Io, "Cannot read keys.dat");

    KeysFile file;
    std::string_view remaining(content.value);
    if (remaining.substr(0, 3) == "\xEF\xBB\xBF")
        remaining.remove_prefix(3);

    // What one section has said so far. The keys stay views into `content`, which outlives them.
    std::string section, label;
    std::string_view signing, encryption;
    bool inSection = false, enabled = true, chan = false, mailingList = false, encrypted = false;
    bool hasSigning = false, hasEncryption = false;
    const auto reset = [&] {
        label.clear();
        signing = encryption = {};
        enabled = true;
        chan = mailingList = encrypted = hasSigning = hasEncryption = false;
    };
    const auto flush = [&] {
        if (!inSection)
            return;
        const auto name = lower(section);
        if (name == "bitmessagesettings") {
            file.encrypted = encrypted;
        } else if (hasSigning || hasEncryption || name.rfind("bm-", 0) == 0) {
            KeyEntry entry;
            entry.address = name.rfind("bm-", 0) == 0 ? "BM-" + section.substr(3) : "BM-" + section;
            entry.label = label;
            entry.chan = chan;
            entry.enabled = enabled;
            entry.mailingList = mailingList;
            if (!hasSigning || !hasEncryption) {
                entry.problem = KeyProblem::MissingKey;
            } else {
                try {
                    entry.signing = decodeWif(signing);
                    entry.encryption = decodeWif(encryption);
                } catch (const std::exception &) {
                    entry.problem = KeyProblem::BadKey;
                }
            }
            file.entries.push_back(std::move(entry));
        }
    };
    reset();
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        auto line =
            text::trim(remaining.substr(0, end == std::string_view::npos ? remaining.size() : end));
        remaining = end == std::string_view::npos ? std::string_view() : remaining.substr(end + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            flush();
            section = std::string(text::trim(line.substr(1, line.size() - 2)));
            inSection = true;
            reset();
            continue;
        }
        // Python's ConfigParser: the option ends at the first ':' or '='.
        const auto delimiter = line.find_first_of(":=");
        if (!inSection || delimiter == std::string_view::npos || delimiter == 0)
            continue;
        const auto option = lower(text::trim(line.substr(0, delimiter)));
        const auto value = text::trim(line.substr(delimiter + 1));
        if (option == "privsigningkey") {
            signing = value;
            hasSigning = !value.empty();
        } else if (option == "privencryptionkey") {
            encryption = value;
            hasEncryption = !value.empty();
        } else if (option == "label") {
            // The parser reads "%%" as one percent sign; the text is UTF-8, or taken as such.
            std::string unescaped;
            for (size_t i = 0; i < value.size(); ++i) {
                unescaped.push_back(value[i]);
                if (value[i] == '%' && i + 1 < value.size() && value[i + 1] == '%')
                    ++i;
            }
            label = text::sanitizeUtf8(unescaped);
        } else if (option == "enabled") {
            enabled = truthy(value, true);
        } else if (option == "chan") {
            chan = truthy(value, false);
        } else if (option == "mailinglist") {
            mailingList = truthy(value, false);
        } else if (option == "keysencrypted") {
            encrypted = truthy(value, false);
        }
    }
    flush();
    return file;
}

SentState sentState(std::string_view status) {
    if (status == "msgsent" || status == "msgsentnoackexpected" || status == "broadcastsent" ||
        status == "ackreceived")
        return SentState::Done;
    if (status == "toodifficult" || status == "badkey")
        return SentState::Failed;
    return SentState::InFlight; // queued, waiting for a key, working on the proof of work, or new
}

MessagesFile::MessagesFile(const std::string &filePath, int busyMilliseconds) {
    if (sqlite3_open_v2(filePath.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        throw Error(Code::Io, "Cannot open the messages.dat of PyBitmessage");
    }
    try {
        // A PyBitmessage that is writing at this moment keeps a reader waiting only briefly.
        sqlite3_busy_timeout(db_, busyMilliseconds);
        const bool ours =
            guarded(db_, [&] { return tableExists(db_, "inbox") && tableExists(db_, "sent"); });
        if (!ours)
            throw Error(Code::Io, "This file is not the messages.dat of PyBitmessage");
        const auto inbox = guarded(db_, [&] { return columnsOf(db_, "inbox"); });
        const auto sent = guarded(db_, [&] { return columnsOf(db_, "sent"); });
        for (const char *column :
             {"msgid", "toaddress", "fromaddress", "subject", "received", "message", "folder"})
            if (!inbox.count(column))
                throw Error(Code::Io, "The messages.dat of PyBitmessage is too old or damaged");
        for (const char *column :
             {"toaddress", "fromaddress", "subject", "message", "ackdata", "senttime", "status"})
            if (!sent.count(column))
                throw Error(Code::Io, "The messages.dat of PyBitmessage is too old or damaged");
        inboxRead_ = inbox.count("read") > 0;
        inboxSighash_ = inbox.count("sighash") > 0;
        sentFolder_ = sent.count("folder") > 0;
        sentMsgid_ = sent.count("msgid") > 0;
    } catch (...) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        throw;
    }
}

MessagesFile::~MessagesFile() {
    sqlite3_close_v2(db_);
}

Census MessagesFile::census() const {
    return guarded(db_, [&] {
        Census c;
        if (tableExists(db_, "settings")) {
            Statement s(db_, "SELECT value FROM settings WHERE CAST(key AS TEXT)='version'");
            int64_t version = 0;
            if (s.row() && seconds(s.text(0), version))
                c.version = int(version);
        }
        {
            Statement s(db_,
                        (std::string("SELECT toaddress, folder, ") +
                         (inboxRead_ ? "\"read\"" : "1") + ", count(*) FROM inbox GROUP BY 1, 2, 3")
                            .c_str());
            std::map<std::tuple<std::string, std::string, bool>, int64_t> groups;
            while (s.row())
                groups[{s.text(0), s.text(1), flag(s, 2)}] += s.number(3);
            for (const auto &[key, count] : groups)
                c.inbox.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), count});
        }
        {
            Statement s(db_, (std::string("SELECT status, ") + (sentFolder_ ? "folder" : "'sent'") +
                              ", count(*) FROM sent GROUP BY 1, 2")
                                 .c_str());
            std::map<std::pair<std::string, std::string>, int64_t> groups;
            while (s.row())
                groups[{s.text(0), s.text(1)}] += s.number(2);
            for (const auto &[key, count] : groups)
                c.sent.push_back({key.first, key.second, count});
        }
        if (tableExists(db_, "subscriptions")) {
            Statement s(db_, "SELECT enabled, count(*) FROM subscriptions GROUP BY 1");
            while (s.row()) {
                c.subscriptions += s.number(1);
                if (!flag(s, 0))
                    c.subscriptionsDisabled += s.number(1);
            }
        }
        const auto count = [&](const char *table) -> int64_t {
            if (!tableExists(db_, table))
                return 0;
            Statement s(db_, (std::string("SELECT count(*) FROM ") + table).c_str());
            return s.row() ? s.number(0) : 0;
        };
        c.contacts = count("addressbook");
        c.blacklisted = count("blacklist");
        c.whitelisted = count("whitelist");
        return c;
    });
}

std::vector<ContactRow> MessagesFile::contacts() const {
    return guarded(db_, [&] {
        std::vector<ContactRow> rows;
        if (!tableExists(db_, "addressbook"))
            return rows;
        Statement s(db_, "SELECT address, label FROM addressbook ORDER BY rowid");
        while (s.row())
            rows.push_back({s.text(0), s.text(1)});
        return rows;
    });
}

std::vector<SubscriptionRow> MessagesFile::subscriptions() const {
    return guarded(db_, [&] {
        std::vector<SubscriptionRow> rows;
        if (!tableExists(db_, "subscriptions"))
            return rows;
        Statement s(db_, "SELECT address, label, enabled FROM subscriptions ORDER BY rowid");
        while (s.row())
            rows.push_back({s.text(0), s.text(1), flag(s, 2)});
        return rows;
    });
}

std::vector<InboxRow> MessagesFile::inbox(int64_t after, int limit) const {
    return guarded(db_, [&] {
        Statement s(db_,
                    (std::string("SELECT rowid, msgid, toaddress, fromaddress, subject, received, "
                                 "message, folder, ") +
                     (inboxRead_ ? "\"read\"" : "1") + ", " + (inboxSighash_ ? "sighash" : "x''") +
                     " FROM inbox WHERE rowid>? ORDER BY rowid LIMIT ?")
                        .c_str());
        s.number(1, after);
        s.number(2, std::clamp(limit, 1, 10000));
        std::vector<InboxRow> rows;
        while (s.row()) {
            InboxRow r;
            r.rowid = s.number(0);
            r.msgid = s.blob(1);
            r.to = s.text(2);
            r.from = s.text(3);
            r.subject = s.text(4);
            r.receivedKnown = seconds(s.text(5), r.received);
            if (!r.receivedKnown)
                r.received = 0;
            r.body = s.text(6);
            r.folder = s.text(7);
            r.read = flag(s, 8);
            r.sighash = s.blob(9);
            rows.push_back(std::move(r));
        }
        return rows;
    });
}

std::vector<SentRow> MessagesFile::sent(int64_t after, int limit) const {
    return guarded(db_, [&] {
        Statement s(db_,
                    (std::string("SELECT rowid, ") + (sentMsgid_ ? "msgid" : "x''") +
                     ", toaddress, fromaddress, subject, message, ackdata, senttime, status, " +
                     (sentFolder_ ? "folder" : "'sent'") +
                     " FROM sent WHERE rowid>? ORDER BY rowid LIMIT ?")
                        .c_str());
        s.number(1, after);
        s.number(2, std::clamp(limit, 1, 10000));
        std::vector<SentRow> rows;
        while (s.row()) {
            SentRow r;
            r.rowid = s.number(0);
            r.msgid = s.blob(1);
            r.to = s.text(2);
            r.from = s.text(3);
            r.subject = s.text(4);
            r.body = s.text(5);
            r.ackdata = s.blob(6);
            if (sqlite3_column_type(s.get(), 7) == SQLITE_INTEGER) {
                r.sent = s.number(7);
            } else {
                r.sentKnown = seconds(s.text(7), r.sent);
                if (!r.sentKnown)
                    r.sent = 0;
            }
            r.status = s.text(8);
            r.folder = s.text(9);
            rows.push_back(std::move(r));
        }
        return rows;
    });
}
} // namespace bmc::pybm
