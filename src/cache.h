#pragma once
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QDirIterator>
#include <memory>
struct sqlite3;
namespace bm {
struct CachedObject {
    qint64 sequence;
    QString hash, path;
};
class Cache {
    sqlite3 *db_ = nullptr;
    QString root_, id_;
    qint64 pruned_ = 0;
    std::unique_ptr<QDirIterator> discovery_;
    QDateTime discoveryModified_;

  public:
    explicit Cache(const QString &root);
    ~Cache();
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    void discover();
    QVector<CachedObject> after(qint64 sequence, int limit = 32) const;
    qint64 count() const;
    qint64 bytes() const;
    qint64 pruned() const {
        return pruned_;
    }
    QString id() const {
        return id_;
    }
    void prune(qint64 maximumBytes = 512ll * 1024 * 1024, int maximumDays = 90);
};
} // namespace bm
