#include "storage.h"
#include "protocol.h"
#include "qt_bridge.h"
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace bm {
static void fail(const QString &m) {
    throw std::runtime_error(m.toStdString());
}
static void check(bool b, const QString &m) {
    if (!b)
        fail(m);
}
QString channelName(const QString &label) {
    return bridge::fromStd(bmc::channelName(bridge::toStd(label)));
}
static void reserveFile(const QString &path) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "File already exists or cannot be created");
    check(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
          "Cannot protect file permissions");
}
std::vector<Identity> Vault::identities() const {
    std::vector<Identity> out;
    for (const auto &i : core_->identities()) {
        Identity q;
        q.label = bridge::fromStd(i.label);
        q.address = bridge::fromStd(i.address);
        q.chan = i.chan;
        q.isDefault = i.isDefault;
        std::memcpy(q.keys.data(), i.keys.data(), 64);
        out.push_back(std::move(q));
    }
    return out;
}
void Vault::create(const QString &path, const QByteArray &password) {
    bridge::SecretText pass(password);
    core_->create(bridge::toStd(path), pass.value);
}
void Vault::unlock(const QString &path, const QByteArray &password) {
    bridge::SecretText pass(password);
    core_->unlock(bridge::toStd(path), pass.value);
}
void Vault::changePassword(const QByteArray &password) {
    bridge::SecretText pass(password);
    core_->changePassword(pass.value);
}
void Vault::lock() {
    core_->lock();
}
QString Vault::addIdentity(const QString &label) {
    return bridge::fromStd(core_->addIdentity(bridge::toStd(label)));
}
QString Vault::addIdentity(const Identity &identity) {
    bmc::Identity core;
    core.label = bridge::toStd(identity.label);
    core.address = bridge::toStd(identity.address);
    core.chan = identity.chan;
    core.isDefault = identity.isDefault;
    std::memcpy(core.keys.data(), identity.keys.data(), 64);
    return bridge::fromStd(core_->addIdentity(core));
}
QString Vault::addChannel(const QString &phrase, const QString &label,
                          const QString &expectedAddress) {
    bridge::SecretText pass(phrase);
    return bridge::fromStd(
        core_->addChannel(pass.value, bridge::toStd(label), bridge::toStd(expectedAddress)));
}
void Vault::importKeys(const QString &path) {
    core_->importKeys(bridge::toStd(path));
}
QString Vault::addMailboxKey() {
    return bridge::fromStd(core_->addMailboxKey());
}
QStringList Vault::mailboxIds() const {
    QStringList ids;
    for (const auto &id : core_->mailboxIds())
        ids << bridge::fromStd(id);
    return ids;
}
const Secret &Vault::mailboxKey(const QString &id) const {
    return core_->mailboxKey(bridge::toStd(id));
}
void Vault::renameIdentity(const QString &address, const QString &label) {
    core_->renameIdentity(bridge::toStd(address), bridge::toStd(label));
}
void Vault::deleteIdentity(const QString &address) {
    core_->deleteIdentity(bridge::toStd(address));
}
void Vault::setDefaultIdentity(const QString &address) {
    core_->setDefaultIdentity(bridge::toStd(address));
}
namespace {
template <class Out, class In> QVector<Out> convertAll(const std::vector<In> &in) {
    QVector<Out> out;
    out.reserve(int(in.size()));
    for (const auto &v : in)
        out.push_back(bridge::fromCore(v));
    return out;
}
} // namespace

void Mailbox::create(const QString &path, const QString &keyId, const Secret &key) {
    core_->create(bridge::toStd(path), bridge::toStd(keyId), key);
}
void Mailbox::open(const QString &path, const Secret &key) {
    core_->open(bridge::toStd(path), key);
}
void Mailbox::close() {
    core_->close();
}
QString Mailbox::keyId() const {
    return bridge::fromStd(core_->keyId());
}
void Mailbox::bindCache(const QString &cacheId) {
    core_->bindCache(bridge::toStd(cacheId));
}
void Mailbox::bindIdentities(const QString &fingerprint, bool keepCheckpoint) {
    core_->bindIdentities(bridge::toStd(fingerprint), keepCheckpoint);
}
QString Mailbox::boundIdentities() const {
    return bridge::fromStd(core_->boundIdentities());
}
Mailbox::Batch::Batch(Mailbox &m) : t_(std::make_unique<bmc::Mailbox::Batch>(*m.core_)) {}
Mailbox::Batch::~Batch() = default;
void Mailbox::Batch::commit() {
    t_->commit();
}
void Mailbox::store(const QString &hash, const QString &from, const QString &to,
                    const QString &subject, const QString &body, qint64 checkpoint,
                    const QString &folder) {
    core_->store(bridge::toStd(hash), bridge::toStd(from), bridge::toStd(to),
                 bridge::toStd(subject), bridge::toStd(body), checkpoint, bridge::toStd(folder));
}
void Mailbox::advance(qint64 checkpoint) {
    core_->advance(checkpoint);
}
qint64 Mailbox::checkpoint() const {
    return core_->checkpoint();
}
QVector<Message> Mailbox::messages() const {
    return convertAll<Message>(core_->messages());
}
QVector<Message> Mailbox::messageSummaries(int limit) const {
    return convertAll<Message>(core_->messageSummaries(limit));
}
QVector<Message> Mailbox::messageSummaries(const QString &folder, const QString &search, int offset,
                                           int limit, const QString &recipient, bool unreadOnly,
                                           bool anonymousOnly) const {
    return convertAll<Message>(core_->messageSummaries(bridge::toStd(folder), bridge::toStd(search),
                                                       offset, limit, bridge::toStd(recipient),
                                                       unreadOnly, anonymousOnly));
}
int Mailbox::messageCount(const QString &folder, const QString &search, const QString &recipient,
                          bool unreadOnly, bool anonymousOnly) const {
    return core_->messageCount(bridge::toStd(folder), bridge::toStd(search),
                               bridge::toStd(recipient), unreadOnly, anonymousOnly);
}
QStringList Mailbox::messageHashes(const QString &folder, const QString &recipient, bool unreadOnly,
                                   bool anonymousOnly) const {
    QStringList hashes;
    for (const auto &h : core_->messageHashes(bridge::toStd(folder), bridge::toStd(recipient),
                                              unreadOnly, anonymousOnly))
        hashes << bridge::fromStd(h);
    return hashes;
}
QVector<Message> Mailbox::messageSummaries(const QStringList &hashes) const {
    std::vector<std::string> ids;
    ids.reserve(size_t(hashes.size()));
    for (const auto &h : hashes)
        ids.push_back(bridge::toStd(h));
    return convertAll<Message>(core_->messageSummaries(ids));
}
int Mailbox::messagePosition(const QString &hash, const QString &folder, const QString &recipient,
                             bool unreadOnly, bool anonymousOnly) const {
    return core_->messagePosition(bridge::toStd(hash), bridge::toStd(folder),
                                  bridge::toStd(recipient), unreadOnly, anonymousOnly);
}
QStringList Mailbox::channelAddresses(const QString &folder) const {
    QStringList addresses;
    for (const auto &a : core_->channelAddresses(bridge::toStd(folder)))
        addresses << bridge::fromStd(a);
    return addresses;
}
bool Mailbox::channelUnread(const QString &recipient, const QString &folder) const {
    return core_->channelUnread(bridge::toStd(recipient), bridge::toStd(folder));
}
void Mailbox::backup(const QString &path, const Secret &key) {
    core_->backup(bridge::toStd(path), key);
}
} // namespace bm
