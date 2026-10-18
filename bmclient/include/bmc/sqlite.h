// A small wrapper over SQLCipher, in std types: a prepared statement and a transaction. The
// mailbox and the cache use it. The Qt app's own copy (src/sqlite_helpers.h) stays for
// src/message_search.cpp, which keeps its own connection.
#pragma once
#include <bmc/bytes.h>
#include <cstdint>
#include <sqlcipher/sqlite3.h>
#include <string>
#include <string_view>

namespace bmc {
class Statement {
    sqlite3_stmt *s_ = nullptr;
    void check(int result) const;

  public:
    Statement(sqlite3 *db, const char *sql);
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    sqlite3_stmt *get() const {
        return s_;
    }
    void text(int n, std::string_view t);
    void number(int n, int64_t i);
    void blob(int n, ByteView b);
    // Ready to bind and step again.
    void reset();
    bool row();
    std::string text(int n) const;
    int64_t number(int n) const;
    Bytes blob(int n) const;
};

// Inside a transaction already open (a scan batch), it joins that one: the outer one commits, or
// rolls everything back if this throws.
class Transaction {
    sqlite3 *db_;
    bool committed_ = false, nested_ = false;

  public:
    explicit Transaction(sqlite3 *db);
    ~Transaction();
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    void commit();
};
} // namespace bmc
