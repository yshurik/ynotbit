#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
struct sqlite3;
namespace bm {
struct CachedObject {
    qint64 sequence;
    QString hash; // lowercase hex, as Protocol::inventoryHash() spells it
    QByteArray payload;
};
// Reads the node's object store (objects.sqlite). Only the node writes it.
class Cache {
    sqlite3 *db_ = nullptr;
    QString root_;
    bool ready();

  public:
    explicit Cache(const QString &root);
    ~Cache();
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    QVector<CachedObject> after(qint64 sequence, int limit = 32);
    bool hasAfter(qint64 sequence);
    qint64 firstSequence(); // 0 when nothing is stored
    QString id();           // empty until the node has created its store
};
} // namespace bm
