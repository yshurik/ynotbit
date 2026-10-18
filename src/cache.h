#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <bmc/cache.h>
#include <memory>
namespace bm {
struct CachedObject {
    qint64 sequence;
    QString hash; // lowercase hex, as Protocol::inventoryHash() spells it
    QByteArray payload;
};
// Reads the node's object store (objects.sqlite). Only the node writes it. The reads and the
// pre-0.5.2 cleanup are bmc::Cache's; this is the Qt-typed veneer over it.
class Cache {
    std::shared_ptr<bmc::Cache> core_;

  public:
    explicit Cache(const QString &root);
    Cache(std::shared_ptr<bmc::Cache> core) : core_(std::move(core)) {}
    ~Cache() = default;
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    bmc::Cache &core() const {
        return *core_;
    }
    const std::shared_ptr<bmc::Cache> &corePtr() const {
        return core_;
    }
    QVector<CachedObject> after(qint64 sequence, int limit = 32);
    bool hasAfter(qint64 sequence);
    qint64 firstSequence(); // 0 when nothing is stored
    QString id();           // empty until the node has created its store
};
} // namespace bm
