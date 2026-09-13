#include "delivery.h"
#include "protocol.h"
#include "protocol_wire.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
namespace bm {
static qint64 now() {
    return QDateTime::currentSecsSinceEpoch();
}
static const Identity *identity(const Vault &vault, const QString &address) {
    for (const auto &i : vault.identities())
        if (i.address == address)
            return &i;
    return nullptr;
}
static bool terminal(const QString &s) {
    return s == "acknowledged" || s == "published" || s == "cancelled" || s == "failed" ||
           s == "expired";
}
static QString addJob(Mailbox &m, QString owner, QString kind, QString address, QByteArray data,
                      quint64 trials = 1000, quint64 extra = 1000, QString state = "queued") {
    for (const auto &j : m.jobs())
        if (j.owner == owner && j.kind == kind && j.address == address)
            return j.id;
    NetworkJob j;
    j.owner = owner;
    j.kind = kind;
    j.address = address;
    j.payload = data;
    j.state = state;
    auto h = Wire::header(data);
    if (!h)
        throw std::runtime_error("Invalid outgoing object");
    j.expires = h->expires;
    j.nonceTrials = std::max<quint64>(1000, trials);
    j.extraBytes = std::max<quint64>(1000, extra);
    if (state == "ready")
        j.hash = Protocol::inventoryHash(data);
    return m.addJob(j);
}
static void saveKey(Mailbox &m, const PublicIdentity &key, qint64 expires) {
    // Ignore extravagant work requests; they must not stall scanning incoming mail.
    if (key.nonceTrials > 1000000 || key.extraBytes > 1000000)
        return;
    m.savePublicKey({key.address, key.signingKey, key.encryptionKey,
                     std::max<quint64>(1000, key.nonceTrials),
                     std::max<quint64>(1000, key.extraBytes), key.behaviors, expires});
}
void Delivery::cancel(Mailbox &m, const QString &id) {
    stop();
    for (const auto &j : m.jobs())
        if (j.owner == id) {
            QFile::remove(root_ + "/publish/" + j.id + ".object");
            QFile::remove(root_ + "/receipts/" + j.id + ".json");
        }
    m.cancel(id);
}
void Delivery::retry(Mailbox &m, const QString &id) {
    stop();
    for (const auto &j : m.jobs())
        if (j.owner == id) {
            QFile::remove(root_ + "/publish/" + j.id + ".object");
            QFile::remove(root_ + "/receipts/" + j.id + ".json");
        }
    m.retry(id, now() + 4 * 86400);
    m.advance(0);
}
void Delivery::plan(Mailbox &m, const Vault &v, qint64 time) {
    for (auto item : m.outbox()) {
        if (terminal(item.state))
            continue;
        if (item.expires <= time) {
            // Finite retries, then an explicit user-action state; never silently discard a letter.
            if (item.attempts < 3) {
                retry(m, item.id);
                continue;
            }
            m.setDelivery(item.id, "expired",
                          "No delivery receipt before expiry. Retry is available.");
            continue;
        }
        if (item.state == "publishing" || item.state == "awaiting_ack" || item.state == "ready")
            continue;
        const auto *sender = identity(v, item.message.from);
        if (!sender) {
            m.setDelivery(item.id, "failed", "Sender identity is missing from this vault");
            continue;
        }
        try {
            if (item.kind == "broadcast") {
                addJob(m, item.id, "broadcast", sender->address,
                       Wire::encodeBroadcast(*sender, item.message.subject, item.message.body,
                                             item.expires));
                if (item.state != "calculating_message")
                    m.setDelivery(item.id, "calculating_message",
                                  "Calculating broadcast proof of work");
                continue;
            }
            std::optional<PublicIdentity> recipient;
            if (auto *local = identity(v, item.message.to))
                recipient = Wire::publicIdentity(*local);
            else if (auto saved = m.publicKey(item.message.to, time))
                recipient = PublicIdentity{saved->address,
                                           saved->signingKey,
                                           saved->encryptionKey,
                                           saved->nonceTrials,
                                           saved->extraBytes,
                                           saved->behaviors,
                                           false};
            if (!recipient) {
                if (item.state != "awaiting_pubkey" || item.nextAttempt <= time) {
                    addJob(m, item.id, "getpubkey", item.message.to,
                           Wire::getPubkey(item.message.to, std::min(item.expires, time + 86400)));
                    m.setDelivery(item.id, "awaiting_pubkey", "Requesting recipient encryption key",
                                  time + 3600);
                }
                continue;
            }
            const bool ack = !recipient->chan && (recipient->behaviors & 1);
            if (ack && item.ackObject.isEmpty()) {
                if (item.ackToken.isEmpty()) {
                    item.ackToken.resize(32);
                    randombytes_buf(item.ackToken.data(), 32);
                    m.setAck(item.id, item.ackToken);
                }
                addJob(m, item.id, "ack-template", item.message.to,
                       Wire::acknowledgment(item.ackToken, item.expires));
                if (item.state != "calculating_ack")
                    m.setDelivery(item.id, "calculating_ack",
                                  "Preparing a private delivery receipt");
                continue;
            }
            addJob(m, item.id, "message", item.message.to,
                   Wire::encodeMessage(*sender, *recipient, item.message.subject, item.message.body,
                                       item.expires, ack ? item.ackObject : QByteArray()),
                   recipient->nonceTrials, recipient->extraBytes);
            if (item.state != "calculating_message")
                m.setDelivery(item.id, "calculating_message",
                              "Encrypting and calculating message proof of work");
        } catch (const std::exception &e) {
            m.setDelivery(item.id, "failed", QString::fromUtf8(e.what()));
        }
    }
}
void Delivery::processJobs(Mailbox &m, bool online) {
    auto jobs = m.jobs();
    if (!mining_.isEmpty()) {
        auto it =
            std::find_if(jobs.begin(), jobs.end(), [&](const auto &j) { return j.id == mining_; });
        if (it == jobs.end())
            stop();
        else if (pow_.done()) {
            auto solved = pow_.take();
            const auto id = mining_;
            mining_.clear();
            m.updateJob(id, "ready", solved, Protocol::inventoryHash(solved));
        }
    }
    for (auto j : m.jobs()) {
        if (j.expires <= now()) {
            QFile::remove(root_ + "/publish/" + j.id + ".object");
            m.removeJob(j.id);
            if (j.id == mining_)
                stop();
            continue;
        }
        if (!j.owner.isEmpty()) {
            const auto state = m.outgoing(j.owner).state;
            if (terminal(state)) {
                QFile::remove(root_ + "/publish/" + j.id + ".object");
                m.removeJob(j.id);
                if (j.id == mining_)
                    stop();
                continue;
            }
        }
        if (j.state == "queued" || j.state == "mining") {
            if (mining_.isEmpty()) {
                pow_.start(j.payload, j.nonceTrials, j.extraBytes);
                mining_ = j.id;
                m.updateJob(j.id, "mining");
            }
            continue;
        }
        if (j.state == "ready" && j.kind == "ack-template") {
            auto item = m.outgoing(j.owner);
            m.setAck(j.owner, item.ackToken, j.payload);
            m.removeJob(j.id);
            continue;
        }
        if (j.state != "ready" && j.state != "submitted")
            continue;
        if (!online)
            continue;
        auto receipt = root_ + "/receipts/" + j.id + ".json",
             spool = root_ + "/publish/" + j.id + ".object";
        QFile f(receipt);
        QJsonObject result;
        if (f.open(QIODevice::ReadOnly))
            result = QJsonDocument::fromJson(f.read(4096)).object();
        auto state = result.value("state").toString();
        if (result.value("hash").toString() != j.hash)
            state.clear();
        if (state == "rejected") {
            if (!j.owner.isEmpty())
                m.setDelivery(j.owner, "failed",
                              "Relay rejected the object; retry to regenerate it");
            m.removeJob(j.id);
            QFile::remove(receipt);
            QFile::remove(spool);
            continue;
        }
        if (state == "offered") {
            if (j.kind == "message" || j.kind == "broadcast") {
                auto item = m.outgoing(j.owner);
                m.setObjectHash(j.owner, j.hash);
                m.setDelivery(
                    j.owner, item.ackObject.isEmpty() ? "published" : "awaiting_ack",
                    item.ackObject.isEmpty()
                        ? "Offered to connected peers. No recipient receipt is available."
                        : "Offered to connected peers; awaiting recipient acknowledgment.");
            }
            if (j.kind == "pubkey")
                m.setSetting("pubkey:" + j.address, QString::number(j.expires));
            m.removeJob(j.id);
            QFile::remove(receipt);
            QFile::remove(spool);
            continue;
        }
        // Recreate an absent job after a relay crash, but never expose ack templates.
        if (!QFile::exists(spool)) {
            QDir().mkpath(root_ + "/publish");
            QSaveFile out(spool);
            if (!out.open(QIODevice::WriteOnly) || out.write(j.payload) != j.payload.size() ||
                !out.commit())
                throw std::runtime_error("Cannot queue encrypted object for relay");
        }
        if (j.state != "submitted") {
            m.updateJob(j.id, "submitted");
            if (j.kind == "message" || j.kind == "broadcast")
                m.setDelivery(j.owner, "publishing",
                              "Waiting for the relay to offer this object to connected peers");
        }
    }
}
void Delivery::tick(Mailbox &m, const Vault &v, bool online) {
    if (!m.isOpen() || !v.unlocked()) {
        stop();
        return;
    }
    plan(m, v, now());
    processJobs(m, online);
}
int Delivery::scan(Cache &cache, Mailbox &m, const Vault &v, int limit) {
    if (!v.unlocked() || !m.isOpen())
        return 0;
    QStringList addresses;
    for (const auto &i : v.identities())
        addresses << i.address;
    for (const auto &s : m.subscriptions())
        addresses << "subscription:" + s.address;
    addresses.sort();
    m.bindIdentities(QString::fromLatin1(
        QCryptographicHash::hash(addresses.join('\n').toUtf8(), QCryptographicHash::Sha256)
            .toHex()));
    int count = 0;
    for (const auto &o : cache.after(m.checkpoint(), limit)) {
        QFile file(o.path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (!file.exists()) {
                m.advance(o.sequence);
                continue;
            }
            throw std::runtime_error("Cannot read a cached object; checkpoint preserved");
        }
        auto data = file.read(262145);
        auto h = Wire::header(data);
        if (!h) {
            m.advance(o.sequence);
            continue;
        }
        auto token = Wire::acknowledgmentToken(data);
        if (!token.isEmpty()) {
            if (m.acknowledge(token))
                ++count;
        }
        if (h->type == 0 && h->expires > now()) {
            for (const auto &i : v.identities())
                if (!i.chan && Wire::requestsIdentity(data, i) &&
                    m.setting("pubkey:" + i.address).toLongLong() < now() + 86400)
                    addJob(m, {}, "pubkey", i.address, Wire::encodePubkey(i, now() + 28 * 86400));
        }
        if (h->type == 1 && h->expires > now()) {
            for (const auto &item : m.outbox())
                if (!terminal(item.state))
                    if (auto key = Wire::decodePubkey(data, item.message.to))
                        saveKey(m, *key, h->expires);
        }
        std::optional<DecodedEnvelope> decoded;
        if (h->type == 2 && token.isEmpty())
            for (const auto &i : v.identities()) {
                decoded = Wire::decodeMessage(data, i);
                if (decoded)
                    break;
            }
        if (h->type == 3)
            for (const auto &sub : m.subscriptions()) {
                decoded = Wire::decodeBroadcast(data, sub.address);
                if (decoded)
                    break;
            }
        if (decoded) {
            auto &d = *decoded;
            auto ackToken = Wire::acknowledgmentToken(d.acknowledgment);
            const auto id =
                ackToken.isEmpty()
                    ? o.hash
                    : QString::fromLatin1(QCryptographicHash::hash(d.message.from.toUtf8() + '\n' +
                                                                       d.message.to.toUtf8() +
                                                                       '\n' + ackToken,
                                                                   QCryptographicHash::Sha256)
                                              .toHex());
            saveKey(m, d.sender, now() + 28 * 86400);
            // Persist ACK job first: retrying this object after a crash remains idempotent.
            if (!d.acknowledgment.isEmpty() && ProofOfWork::valid(d.acknowledgment, now()))
                addJob(m, {}, "incoming-ack", Protocol::inventoryHash(d.acknowledgment),
                       d.acknowledgment, 1000, 1000, "ready");
            m.store(id, d.message.from, d.message.to, d.message.subject, d.message.body, o.sequence,
                    d.broadcast ? "Broadcasts" : d.message.folder);
            ++count;
        } else
            m.advance(o.sequence);
    }
    return count;
}
} // namespace bm
