// The mailbox's letters, deliveries, jobs, keys, subscriptions, contacts and settings: a thin
// wrapper over bmc::Mailbox (bmclient), which holds the SQL. Each body converts its arguments and
// its results.
#include "storage.h"
#include "qt_bridge.h"
#include <algorithm>
namespace bm {
namespace {
template <class Out, class In> QVector<Out> convertAll(const std::vector<In> &in) {
    QVector<Out> out;
    out.reserve(int(in.size()));
    for (const auto &v : in)
        out.push_back(bridge::fromCore(v));
    return out;
}
} // namespace

QString Mailbox::saveDraft(const QString &id, const QString &from, const QString &to,
                           const QString &subject, const QString &body) {
    return bridge::fromStd(core_->saveDraft(bridge::toStd(id), bridge::toStd(from), bridge::toStd(to),
                                            bridge::toStd(subject), bridge::toStd(body)));
}
Message Mailbox::message(const QString &id) const {
    return bridge::fromCore(core_->message(bridge::toStd(id)));
}
void Mailbox::queueDraft(const QString &id, const QString &kind, qint64 expires) {
    core_->queueDraft(bridge::toStd(id), bridge::toStd(kind), expires);
}
QVector<OutboxItem> Mailbox::outbox() const {
    return convertAll<OutboxItem>(core_->outbox());
}
OutboxItem Mailbox::outgoing(const QString &id) const {
    return bridge::fromCore(core_->outgoing(bridge::toStd(id)));
}
void Mailbox::setDelivery(const QString &id, const QString &state, const QString &detail,
                          qint64 nextAttempt) {
    core_->setDelivery(bridge::toStd(id), bridge::toStd(state), bridge::toStd(detail), nextAttempt);
}
void Mailbox::setAck(const QString &id, const QByteArray &token, const QByteArray &object) {
    core_->setAck(bridge::toStd(id), bridge::view(token), bridge::view(object));
}
void Mailbox::setObjectHash(const QString &id, const QString &hash) {
    core_->setObjectHash(bridge::toStd(id), bridge::toStd(hash));
}
bool Mailbox::acknowledge(const QByteArray &token) {
    return core_->acknowledge(bridge::view(token));
}
void Mailbox::retry(const QString &id, qint64 expires) {
    core_->retry(bridge::toStd(id), expires);
}
void Mailbox::cancel(const QString &id) {
    core_->cancel(bridge::toStd(id));
}
QVector<DeliveryEvent> Mailbox::events(const QString &id) const {
    return convertAll<DeliveryEvent>(core_->events(bridge::toStd(id)));
}
void Mailbox::recordMilestone(const QString &id, const QString &state, const QString &detail) {
    core_->recordMilestone(bridge::toStd(id), bridge::toStd(state), bridge::toStd(detail));
}
QString Mailbox::addJob(NetworkJob job) {
    return bridge::fromStd(core_->addJob(bridge::toCore(job)));
}
QVector<NetworkJob> Mailbox::jobs() const {
    return convertAll<NetworkJob>(core_->jobs());
}
void Mailbox::updateJob(const QString &id, const QString &state, const QByteArray &payload,
                        const QString &hash, const QString &error) {
    // A null payload means "leave the stored one"; the core says that with a missing pointer.
    const bmc::Bytes bytes = payload.isNull() ? bmc::Bytes() : bridge::toBytes(payload);
    core_->updateJob(bridge::toStd(id), bridge::toStd(state), payload.isNull() ? nullptr : &bytes,
                     bridge::toStd(hash), bridge::toStd(error));
}
void Mailbox::removeJob(const QString &id) {
    core_->removeJob(bridge::toStd(id));
}
void Mailbox::savePublicKey(const StoredPublicKey &key) {
    core_->savePublicKey(bridge::toCore(key));
}
std::optional<StoredPublicKey> Mailbox::publicKey(const QString &address, qint64 now) const {
    const auto key = core_->publicKey(bridge::toStd(address), now);
    return key ? std::optional<StoredPublicKey>(bridge::fromCore(*key)) : std::nullopt;
}
QVector<Subscription> Mailbox::subscriptions() const {
    return convertAll<Subscription>(core_->subscriptions());
}
void Mailbox::subscribe(const QString &address, const QString &label) {
    core_->subscribe(bridge::toStd(address), bridge::toStd(label));
}
void Mailbox::unsubscribe(const QString &address) {
    core_->unsubscribe(bridge::toStd(address));
}
QVector<Contact> Mailbox::contacts() const {
    return convertAll<Contact>(core_->contacts());
}
void Mailbox::saveContact(const QString &address, const QString &label) {
    core_->saveContact(bridge::toStd(address), bridge::toStd(label));
}
void Mailbox::removeContact(const QString &address) {
    core_->removeContact(bridge::toStd(address));
}
void Mailbox::moveMessage(const QString &id, const QString &folder) {
    core_->moveMessage(bridge::toStd(id), bridge::toStd(folder));
}
void Mailbox::restoreMessage(const QString &id) {
    core_->restoreMessage(bridge::toStd(id));
}
void Mailbox::deleteMessage(const QString &id) {
    core_->deleteMessage(bridge::toStd(id));
}
void Mailbox::markRead(const QString &id) {
    core_->markRead(bridge::toStd(id));
}
int Mailbox::markRead(const QStringList &ids) {
    std::vector<std::string> hashes;
    hashes.reserve(size_t(ids.size()));
    for (const auto &id : ids)
        hashes.push_back(bridge::toStd(id));
    return core_->markRead(hashes);
}
bool Mailbox::unread(const QString &id) const {
    return core_->unread(bridge::toStd(id));
}
QString Mailbox::setting(const QString &name, const QString &fallback) const {
    return bridge::fromStd(core_->setting(bridge::toStd(name), bridge::toStd(fallback)));
}
void Mailbox::setSetting(const QString &name, const QString &value) {
    core_->setSetting(bridge::toStd(name), bridge::toStd(value));
}
} // namespace bm
