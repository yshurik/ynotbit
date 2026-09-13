#pragma once
#include <QByteArray>
#include <QString>
#include <sqlite3.h>
#include <stdexcept>
namespace bm::detail {
class Statement {
    sqlite3_stmt *s_ = nullptr;
    void check(int result) const {
        if (result != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s_)));
    }

  public:
    Statement(sqlite3 *db, const char *sql) {
        if (!db)
            throw std::runtime_error("Mailbox is closed");
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() {
        sqlite3_finalize(s_);
    }
    Statement(const Statement &) = delete;
    sqlite3_stmt *get() const {
        return s_;
    }
    void text(int n, const QString &t) {
        auto b = t.toUtf8();
        check(sqlite3_bind_text(s_, n, b.constData(), b.size(), SQLITE_TRANSIENT));
    }
    void number(int n, qint64 i) {
        check(sqlite3_bind_int64(s_, n, i));
    }
    void blob(int n, const QByteArray &b) {
        check(sqlite3_bind_blob(s_, n, b.constData(), b.size(), SQLITE_TRANSIENT));
    }
    bool row() {
        int r = sqlite3_step(s_);
        if (r == SQLITE_ROW)
            return true;
        if (r != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s_)));
        return false;
    }
    QString text(int n) const {
        auto p = sqlite3_column_text(s_, n);
        return p ? QString::fromUtf8(reinterpret_cast<const char *>(p), sqlite3_column_bytes(s_, n))
                 : QString();
    }
    qint64 number(int n) const {
        return sqlite3_column_int64(s_, n);
    }
    QByteArray blob(int n) const {
        return QByteArray(static_cast<const char *>(sqlite3_column_blob(s_, n)),
                          sqlite3_column_bytes(s_, n));
    }
};
class Transaction {
    sqlite3 *db_;
    bool committed_ = false;

  public:
    explicit Transaction(sqlite3 *db) : db_(db) {
        if (!db || sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(db ? sqlite3_errmsg(db) : "Mailbox is closed");
    }
    ~Transaction() {
        if (!committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
        committed_ = true;
    }
};
} // namespace bm::detail
