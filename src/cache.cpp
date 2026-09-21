#include "cache.h"
#include "protocol.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <sqlcipher/sqlite3.h>
#include <stdexcept>
namespace bm {
static void check(bool v) {
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
    void text(int n, const QString &s) {
        auto b = s.toUtf8();
        check(sqlite3_bind_text(p, n, b.data(), b.size(), SQLITE_TRANSIENT) == SQLITE_OK);
    }
    void num(int n, qint64 i) {
        sqlite3_bind_int64(p, n, i);
    }
    bool row() {
        auto r = sqlite3_step(p);
        check(r == SQLITE_ROW || r == SQLITE_DONE);
        return r == SQLITE_ROW;
    }
    void reset() {
        sqlite3_reset(p);
        sqlite3_clear_bindings(p);
    }
    QString text(int n) const {
        return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(p, n)));
    }
    qint64 num(int n) const {
        return sqlite3_column_int64(p, n);
    }
};
Cache::Cache(const QString &root) : root_(root) {
    check(QDir().mkpath(root + "/objects"));
    auto path = QFile::encodeName(root + "/cache.sqlite");
    if (sqlite3_open(path.constData(), &db_) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        check(false);
    }
    sqlite3_busy_timeout(db_, 3000);
    check(sqlite3_exec(
              db_,
              "PRAGMA journal_mode=DELETE; CREATE TABLE IF NOT EXISTS cache_id(id TEXT); CREATE "
              "TABLE IF NOT EXISTS objects(seq INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT UNIQUE "
              "NOT NULL, size INTEGER NOT NULL, received INTEGER NOT NULL);",
              nullptr, nullptr, nullptr) == SQLITE_OK);
    Stmt s(db_, "SELECT id FROM cache_id");
    if (s.row())
        id_ = s.text(0);
    else {
        id_ = QUuid::createUuid().toString();
        Stmt insert(db_, "INSERT INTO cache_id VALUES(?)");
        insert.text(1, id_);
        insert.row();
    }
}
Cache::~Cache() {
    sqlite3_close_v2(db_);
}
void Cache::discover() {
    static QRegularExpression valid("^[a-f0-9]{64}$");
    // QDirIterator snapshots the directory at construction: files written after
    // that are invisible to it no matter how many more times it's resumed. An
    // earlier version of this function rebuilt the iterator on every call where
    // the directory's mtime had changed since construction, to avoid a file
    // added mid-backlog staying invisible until that backlog finished draining.
    // Under sustained write traffic (a busy relay node) the directory's mtime
    // changes on nearly every call, so that reset fired almost every time and
    // the walk could never progress past whatever the first ~128 entries were --
    // objects arriving later in iteration order could stay unregistered
    // indefinitely, silently dropped from the mailbox scan that reads this
    // table. Instead, let the current snapshot run to completion (bounded below
    // at 128 entries / 5ms per call, resumed across calls); once it's fully
    // drained, the next call builds a fresh iterator that picks up everything
    // written since, including anything added mid-pass.
    if (!discovery_)
        discovery_ =
            std::make_unique<QDirIterator>(root_ + "/objects", QDir::Files | QDir::NoSymLinks);
    int examined = 0;
    QElapsedTimer budget;
    budget.start();
    // Batch this call's inserts into one transaction. Without it, each INSERT OR
    // IGNORE autocommits separately, and with journal_mode=DELETE that's a
    // filesystem sync per row -- measured at roughly one object registered per
    // discover() call, nowhere near enough to keep up with a busy node's real
    // write rate. Confirmed against a real user's node directory: 4405 of
    // 17486 cached object files (25%) were never registered despite existing
    // correctly on disk, because discovery could never catch up.
    check(sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    try {
        Stmt known(db_, "SELECT 1 FROM objects WHERE hash=?");
        Stmt insert(db_, "INSERT OR IGNORE INTO objects(hash,size,received) VALUES(?,?,?)");
        while (discovery_->hasNext() && examined++ < 128 && budget.elapsed() < 5) {
            discovery_->next();
            const auto f = discovery_->fileInfo();
            if (!valid.match(f.fileName()).hasMatch())
                continue;
            known.text(1, f.fileName());
            const bool isKnown = known.row();
            known.reset();
            if (isKnown)
                continue;
            if (f.size() < 22 || f.size() > 262144)
                continue;
            QFile file(f.filePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            auto object = file.readAll();
            if (Protocol::inventoryHash(object) != f.fileName())
                continue;
            insert.text(1, f.fileName());
            insert.num(2, f.size());
            insert.num(3, QDateTime::currentSecsSinceEpoch());
            insert.row();
            insert.reset();
        }
        check(sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    if (!discovery_->hasNext())
        discovery_.reset();
}
QVector<CachedObject> Cache::after(qint64 sequence, int limit) const {
    Stmt s(db_, "SELECT seq,hash FROM objects WHERE seq>? ORDER BY seq LIMIT ?");
    s.num(1, sequence);
    s.num(2, limit);
    QVector<CachedObject> list;
    while (s.row())
        list.push_back({s.num(0), s.text(1), root_ + "/objects/" + s.text(1)});
    return list;
}
qint64 Cache::count() const {
    Stmt s(db_, "SELECT count(*) FROM objects");
    s.row();
    return s.num(0);
}
qint64 Cache::bytes() const {
    Stmt s(db_, "SELECT coalesce(sum(size),0) FROM objects");
    s.row();
    return s.num(0);
}
void Cache::prune(qint64 maximumBytes, int days) {
    // A paused discover() iterator may still be walking entries this call is about
    // to delete, and won't see files written after it started either; drop it so
    // the next discover() re-scans the directory as it stands now.
    discovery_.reset();
    auto used = bytes(), threshold = QDateTime::currentSecsSinceEpoch() - qint64(days) * 86400;
    Stmt s(db_, "SELECT seq,hash,size,received FROM objects ORDER BY seq");
    while (s.row()) {
        if (used <= maximumBytes && s.num(3) >= threshold)
            break;
        auto path = root_ + "/objects/" + s.text(1);
        if (QFile::exists(path) && !QFile::remove(path))
            continue;
        Stmt del(db_, "DELETE FROM objects WHERE seq=?");
        del.num(1, s.num(0));
        del.row();
        used -= s.num(2);
        ++pruned_;
    }
}
} // namespace bm
