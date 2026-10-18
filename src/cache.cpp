#include "cache.h"
#include "qt_bridge.h"
namespace bm {
Cache::Cache(const QString &root) : core_(std::make_shared<bmc::Cache>(bridge::toStd(root))) {}
QVector<CachedObject> Cache::after(qint64 sequence, int limit) {
    QVector<CachedObject> list;
    for (const auto &o : core_->after(sequence, limit))
        list.push_back({o.sequence, bridge::fromStd(o.hash), bridge::fromBytes(o.payload)});
    return list;
}
bool Cache::hasAfter(qint64 sequence) {
    return core_->hasAfter(sequence);
}
qint64 Cache::firstSequence() {
    return core_->firstSequence();
}
QString Cache::id() {
    return bridge::fromStd(core_->id());
}
} // namespace bm
