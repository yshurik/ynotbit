#include "cache.h"
#include "ntb-object-db.h"
#include <QFile>
#include <sqlcipher/sqlite3.h>
#include <stdexcept>
namespace bm {
namespace {
void check(bool v) {
    if (!v)
        throw std::runtime_error("Network cache database error");
}
struct Stmt {
    sqlite3_stmt *p = nullptr;
    Stmt(sqlite3 *d, const char *q) {
        check(sqlite3_prepare_v2(d, q, -1, &p, nullptr) == SQLITE_OK);
    }
    ~Stmt() {
        sqlite3_finalize(p);
    }
    void num(int n, qint64 i) {
        sqlite3_bind_int64(p, n, i);
    }
    bool row() {
        auto r = sqlite3_step(p);
        check(r == SQLITE_ROW || r == SQLITE_DONE);
        return r == SQLITE_ROW;
    }
    qint64 num(int n) const {
        return sqlite3_column_int64(p, n);
    }
    QByteArray blob(int n) const {
        return QByteArray(static_cast<const char *>(sqlite3_column_blob(p, n)),
                          sqlite3_column_bytes(p, n));
    }
    QString text(int n) const {
        return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(p, n)));
    }
};
} // namespace
Cache::Cache(const QString &root) : root_(root) {
    // Before 0.5.2 the app kept its own index of the node's objects/ folder.
    QFile::remove(root + "/cache.sqlite");
    QFile::remove(root + "/cache.sqlite-journal");
}
Cache::~Cache() {
    sqlite3_close_v2(db_);
}
bool Cache::ready() {
    if (!db_) {
        const auto path = root_ + "/" NTB_OBJECT_DB_FILE;
        if (!QFile::exists(path))
            return false;
        // Read-write so the WAL index can be updated; never CREATE: only the
        // node makes this file.
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db_, SQLITE_OPEN_READWRITE,
                            nullptr) != SQLITE_OK) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
            return false;
        }
        sqlite3_busy_timeout(db_, 1000);
    }
    // Zero while the node is creating or emptying the store.
    Stmt version(db_, "PRAGMA user_version");
    return version.row() && version.num(0) == NTB_OBJECT_DB_VERSION;
}
QVector<CachedObject> Cache::after(qint64 sequence, int limit) {
    QVector<CachedObject> list;
    if (!ready())
        return list;
    Stmt s(db_, "SELECT seq,hash,payload FROM objects WHERE seq>? ORDER BY seq LIMIT ?");
    s.num(1, sequence);
    s.num(2, limit);
    while (s.row())
        list.push_back({s.num(0), QString::fromLatin1(s.blob(1).toHex()), s.blob(2)});
    return list;
}
bool Cache::hasAfter(qint64 sequence) {
    if (!ready())
        return false;
    Stmt s(db_, "SELECT 1 FROM objects WHERE seq>? LIMIT 1");
    s.num(1, sequence);
    return s.row();
}
qint64 Cache::firstSequence() {
    if (!ready())
        return 0;
    Stmt s(db_, "SELECT coalesce(min(seq),0) FROM objects");
    s.row();
    return s.num(0);
}
QString Cache::id() {
    if (!ready())
        return {};
    Stmt s(db_, "SELECT value FROM meta WHERE key='cache_id'");
    return s.row() ? s.text(0) : QString();
}
} // namespace bm
