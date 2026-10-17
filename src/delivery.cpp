#include "delivery.h"
#include "qt_bridge.h"
#include <bmc/clock.h>
namespace bm {
Delivery::Delivery(QString root) : core_(std::make_shared<bmc::Delivery>(bridge::toStd(root))) {}
void Delivery::stop() {
    core_->stop();
}
void Delivery::cancel(Mailbox &m, const QString &id) {
    core_->cancel(m.core(), bridge::toStd(id));
}
void Delivery::retry(Mailbox &m, const QString &id) {
    core_->retry(m.core(), bridge::toStd(id));
}
int Delivery::scan(Cache &cache, Mailbox &m, const Vault &v, int limit, int budgetMs) {
    return core_->scan(cache.core(), m.core(), v.core(), bmc::now(), limit, budgetMs);
}
void Delivery::rereadKept(Mailbox &m) {
    bmc::Delivery::rereadKept(m.core());
}
void Delivery::tick(Mailbox &m, const Vault &v, bool online) {
    core_->tick(m.core(), v.core(), online, bmc::now());
}
bool Delivery::working() const {
    return core_->working();
}
} // namespace bm
