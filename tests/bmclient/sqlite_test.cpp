// The SQLCipher wrapper on its own: a statement's binders and its reset, and a transaction that
// commits, rolls back or joins the one already open. The mailbox reaches all of this through its
// own methods, but never with the failures these paths exist for — a parameter number that is not
// there, a step the database refuses, a COMMIT that cannot be done — so they are asked directly
// here, against a database of the test's own.
#include <bmc/error.h>
#include <bmc/sqlite.h>
#include <bmc/test.h>
#include <sqlcipher/sqlite3.h>

namespace {
struct Db {
    sqlite3 *db = nullptr;
    explicit Db(const std::string &name = "s.db") {
        const auto path = bmc::test::tempDir() + "/" + name;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK)
            bmc::test::fail(__FILE__, __LINE__, "cannot open a database");
        exec("PRAGMA key='a test key'"); // before anything else touches the file, as the mailbox
                                         // does
        exec("CREATE TABLE t(k TEXT NOT NULL UNIQUE, n INTEGER, b BLOB)");
    }
    ~Db() {
        sqlite3_close(db);
    }
    void exec(const char *sql) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            bmc::test::fail(__FILE__, __LINE__, sqlite3_errmsg(db));
    }
    int64_t count() {
        bmc::Statement s(db, "SELECT count(*) FROM t");
        s.row();
        return s.number(0);
    }
};
} // namespace

TEST(a_statement_binds_every_type_and_resets) {
    Db db;
    bmc::Statement insert(db.db, "INSERT INTO t(k,n,b) VALUES(?,?,?)");
    insert.text(1, "");
    insert.number(2, -7);
    insert.blob(3, bmc::unhex("00ff"));
    CHECK(!insert.row()); // an INSERT has no rows, only an end
    bmc::Statement out(db.db, "SELECT k,n,b FROM t");
    CHECK(out.row());
    CHECK_EQ(out.text(0), std::string(""));
    CHECK_EQ(out.number(1), int64_t(-7));
    CHECK_EQ(bmc::hex(out.blob(2)), std::string("00ff"));
    CHECK(!out.row());
    // An empty string and an empty blob land as empty, not as NULL: the columns the app binds them
    // to are NOT NULL, and Qt's toUtf8().constData() was never null either.
    bmc::Statement empty(db.db, "INSERT INTO t(k,n,b) VALUES(?,?,?)");
    empty.text(1, "a row of empty values");
    empty.number(2, 0);
    empty.blob(3, bmc::ByteView());
    CHECK(!empty.row());
    bmc::Statement nulls(db.db, "SELECT k IS NULL, b IS NULL FROM t WHERE n=0");
    CHECK(nulls.row());
    CHECK_EQ(nulls.number(0), int64_t(0));
    CHECK_EQ(nulls.number(1), int64_t(0));
    // reset() puts a statement back to its start, ready to bind and step again.
    bmc::Statement again(db.db, "SELECT count(*) FROM t WHERE n=?");
    again.number(1, -7);
    CHECK(again.row());
    CHECK_EQ(again.number(0), int64_t(1));
    again.reset();
    again.number(1, 99);
    CHECK(again.row());
    CHECK_EQ(again.number(0), int64_t(0));
    // A parameter number that is not in the statement is refused, not stepped over.
    CHECK_THROWS(again.number(9, 1));
    CHECK_THROWS(again.text(9, "x"));
    CHECK_THROWS(again.blob(9, bmc::unhex("00")));
    // A step the database refuses throws rather than looking like the end of the rows, and a
    // statement with no database behind it says which.
    bmc::Statement duplicate(db.db, "INSERT INTO t(k,n,b) VALUES('',9,x'')");
    CHECK_THROWS(duplicate.row());
    CHECK_THROWS(bmc::Statement(nullptr, "SELECT 1"));
    CHECK_THROWS(bmc::Statement(db.db, "SELECT * FROM nowhere"));
}

TEST(a_transaction_commits_rolls_back_or_joins_the_outer_one) {
    Db db;
    {
        bmc::Transaction t(db.db);
        db.exec("INSERT INTO t(k,n,b) VALUES('committed',1,x'')");
        t.commit();
    }
    CHECK_EQ(db.count(), int64_t(1));
    {
        bmc::Transaction t(db.db);
        db.exec("INSERT INTO t(k,n,b) VALUES('dropped',2,x'')");
    } // no commit: the destructor rolls it back
    CHECK_EQ(db.count(), int64_t(1));
    // Inside a transaction already open, this one joins it: what it holds is not committed on its
    // own, but what the outer one does comes through.
    db.exec("BEGIN IMMEDIATE");
    {
        bmc::Transaction nested(db.db);
        db.exec("INSERT INTO t(k,n,b) VALUES('nested',3,x'')");
        nested.commit(); // nothing of its own to commit
        CHECK_EQ(db.count(), int64_t(2));
    }
    db.exec("COMMIT");
    CHECK_EQ(db.count(), int64_t(2));
    // A transaction with no database behind it, and a COMMIT the database will not do: a deferred
    // constraint is not checked until then, and a COMMIT that fails is an error, not a success.
    CHECK_THROWS(bmc::Transaction(nullptr));
    db.exec("CREATE TABLE child(x TEXT REFERENCES t(k) DEFERRABLE INITIALLY DEFERRED)");
    db.exec("PRAGMA foreign_keys=ON");
    bmc::Transaction bad(db.db);
    db.exec("INSERT INTO child(x) VALUES('no such row')");
    try {
        bad.commit();
        bmc::test::fail(__FILE__, __LINE__, "a COMMIT the database refused looked like a success");
    } catch (const bmc::Error &e) {
        CHECK_EQ(int(e.code()), int(bmc::Code::Io));
    }
    CHECK_EQ(db.count(), int64_t(2));
}
