#include <bmc/error.h>
#include <bmc/sqlite.h>
#include <cstring>

namespace bmc {
void Statement::check(int result) const {
    if (result != SQLITE_OK)
        throw Error(Code::Io, sqlite3_errmsg(sqlite3_db_handle(s_)));
}

Statement::Statement(sqlite3 *db, const char *sql) {
    if (!db)
        throw Error(Code::State, "Mailbox is closed");
    if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
        throw Error(Code::Io, sqlite3_errmsg(db));
}

Statement::~Statement() {
    sqlite3_finalize(s_);
}

void Statement::text(int n, std::string_view t) {
    // Qt's toUtf8().constData() is never null, so an empty string binds as an empty
    // TEXT, not as NULL: the columns it lands in are NOT NULL.
    static const char empty = 0;
    check(sqlite3_bind_text(s_, n, t.empty() ? &empty : t.data(), int(t.size()), SQLITE_TRANSIENT));
}

void Statement::number(int n, int64_t i) {
    check(sqlite3_bind_int64(s_, n, i));
}

void Statement::blob(int n, ByteView b) {
    static const uint8_t empty = 0;
    check(sqlite3_bind_blob(s_, n, b.empty() ? &empty : b.data(), int(b.size()), SQLITE_TRANSIENT));
}

void Statement::reset() {
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
}

bool Statement::row() {
    const int r = sqlite3_step(s_);
    if (r == SQLITE_ROW)
        return true;
    if (r != SQLITE_DONE)
        throw Error(Code::Io, sqlite3_errmsg(sqlite3_db_handle(s_)));
    return false;
}

std::string Statement::text(int n) const {
    const auto *p = sqlite3_column_text(s_, n);
    return p ? std::string(reinterpret_cast<const char *>(p), size_t(sqlite3_column_bytes(s_, n)))
             : std::string();
}

int64_t Statement::number(int n) const {
    return sqlite3_column_int64(s_, n);
}

Bytes Statement::blob(int n) const {
    const auto *p = static_cast<const uint8_t *>(sqlite3_column_blob(s_, n));
    const int size = sqlite3_column_bytes(s_, n);
    return p ? Bytes(p, p + size) : Bytes();
}

Transaction::Transaction(sqlite3 *db) : db_(db) {
    if (!db)
        throw Error(Code::State, "Mailbox is closed");
    nested_ = sqlite3_get_autocommit(db) == 0;
    if (!nested_ && sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw Error(Code::Io, sqlite3_errmsg(db));
}

Transaction::~Transaction() {
    if (!committed_ && !nested_)
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}

void Transaction::commit() {
    if (!nested_ && sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw Error(Code::Io, sqlite3_errmsg(db_));
    committed_ = true;
}
} // namespace bmc
