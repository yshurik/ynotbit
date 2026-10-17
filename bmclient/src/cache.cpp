#include <algorithm>
#include <bmc/cache.h>
#include <bmc/error.h>
#include <bmc/hash.h>
#include <bmc/mailbox.h>
#include <bmc/path.h>
#include <bmc/protocol.h>
#include <bmc/sqlite.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <filesystem>
#include <ntb-object-db.h>
#include <sqlcipher/sqlite3.h>

namespace bmc {
namespace {
// A store the node is writing, or one that is not a store at all, reports the message the app has
// always shown for it, not SQLite's own. It always throws, so the return after a catch is dead.
void unreadable() {
    require(false, "Network cache database error");
}
std::string join(const std::vector<std::string> &parts, char separator) {
    std::string out;
    for (const auto &part : parts) {
        if (!out.empty())
            out.push_back(separator);
        out += part;
    }
    return out;
}
} // namespace

Cache::Cache(std::string root) : root_(std::move(root)) {
    // Before 0.5.2 the app kept its own index of the node's objects/ folder.
    std::error_code ignored;
    std::filesystem::remove(path(root_ + "/cache.sqlite"), ignored);
    std::filesystem::remove(path(root_ + "/cache.sqlite-journal"), ignored);
}

Cache::~Cache() {
    sqlite3_close_v2(db_);
}

bool Cache::ready() {
    if (!db_) {
        const auto file = root_ + "/" NTB_OBJECT_DB_FILE;
        std::error_code
            ignored; // a path that cannot be probed is not a store, as QFile::exists said
        if (!std::filesystem::exists(path(file), ignored))
            return false;
        // Read-write so the WAL index can be updated; never CREATE: only the
        // node makes this file.
        if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
            return false;
        }
        sqlite3_busy_timeout(db_, 1000);
    }
    // Zero while the node is creating or emptying the store.
    try {
        Statement version(db_, "PRAGMA user_version");
        return version.row() && version.number(0) == NTB_OBJECT_DB_VERSION;
    } catch (const std::exception &) {
        unreadable();
    }
    return false;
}

std::vector<CachedObject> Cache::after(int64_t sequence, int limit) {
    std::vector<CachedObject> list;
    if (!ready())
        return list;
    try {
        Statement s(db_, "SELECT seq,hash,payload FROM objects WHERE seq>? ORDER BY seq LIMIT ?");
        s.number(1, sequence);
        s.number(2, limit);
        while (s.row())
            list.push_back({s.number(0), hex(s.blob(1)), s.blob(2)});
    } catch (const std::exception &) {
        unreadable();
    }
    return list;
}

bool Cache::hasAfter(int64_t sequence) {
    if (!ready())
        return false;
    try {
        Statement s(db_, "SELECT 1 FROM objects WHERE seq>? LIMIT 1");
        s.number(1, sequence);
        return s.row();
    } catch (const std::exception &) {
        unreadable();
    }
    return false;
}

int64_t Cache::firstSequence() {
    if (!ready())
        return 0;
    try {
        Statement s(db_, "SELECT coalesce(min(seq),0) FROM objects");
        s.row();
        return s.number(0);
    } catch (const std::exception &) {
        unreadable();
    }
    return 0;
}

std::string Cache::id() {
    if (!ready())
        return {};
    try {
        Statement s(db_, "SELECT value FROM meta WHERE key='cache_id'");
        return s.row() ? s.text(0) : std::string();
    } catch (const std::exception &) {
        unreadable();
    }
    return {};
}

int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit) {
    if (!vault.unlocked() || !mailbox.isOpen())
        return 0;
    std::vector<std::string> addresses;
    for (const auto &identity : vault.identities())
        addresses.push_back(identity.address);
    std::sort(addresses.begin(), addresses.end());
    mailbox.bindIdentities(hex(hash(Digest::Sha256, view(join(addresses, '\n')))));
    int decodedCount = 0;
    for (const auto &o : cache.after(mailbox.checkpoint(), limit)) {
        if (o.payload.size() > NTB_OBJECT_DB_MAX_OBJECT_SIZE) {
            mailbox.advance(o.sequence);
            continue;
        }
        const auto data = ByteView(o.payload);
        bool found = false;
        for (const auto &i : vault.identities()) {
            auto decoded = Protocol::decodeMessage(data, IdentityRef(i));
            if (decoded) {
                const auto header = Wire::header(data);
                if (header)
                    mailbox.storeRouted(o.hash, decoded->from, decoded->to, decoded->subject,
                                        decoded->body, o.sequence, decoded->folder,
                                        header->expires);
                else
                    mailbox.advance(o.sequence);
                found = true;
                ++decodedCount;
                break;
            }
        }
        if (!found)
            mailbox.advance(o.sequence);
    }
    return decodedCount;
}
} // namespace bmc
