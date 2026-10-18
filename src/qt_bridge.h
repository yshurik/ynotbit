// The Qt wrappers in src/ call the core (bmclient, which has no Qt) through these conversions.
#pragma once
#include "protocol_wire.h"
#include "storage.h"
#include <QByteArray>
#include <QString>
#include <bmc/bytes.h>
#include <bmc/identity.h>
#include <bmc/wire.h>
#include <sodium.h>

namespace bm::bridge {
// QString to UTF-8 (a lone surrogate becomes '?', as QString::toUtf8() makes it) and back.
inline std::string toStd(const QString &text) {
    return text.toUtf8().toStdString();
}
inline QString fromStd(std::string_view text) {
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}
inline bmc::ByteView view(const QByteArray &bytes) {
    return {reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())};
}
inline bmc::Bytes toBytes(const QByteArray &bytes) {
    const auto v = view(bytes);
    return bmc::Bytes(v.begin(), v.end());
}
inline QByteArray fromBytes(bmc::ByteView bytes) {
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
}
// A bm::Identity as the core reads it: the address copied to UTF-8, the keys borrowed.
class CoreIdentity {
  public:
    explicit CoreIdentity(const Identity &i)
        : address_(toStd(i.address)), chan_(i.chan), keys_(i.keys.data()) {}
    operator bmc::IdentityRef() const {
        return {address_, chan_, keys_};
    }

  private:
    std::string address_;
    bool chan_;
    const unsigned char *keys_;
};
inline Identity fromCore(bmc::Identity &&core) {
    Identity i;
    i.label = fromStd(core.label);
    i.address = fromStd(core.address);
    i.chan = core.chan;
    i.isDefault = core.isDefault;
    i.keys = std::move(core.keys);
    return i;
}
inline Message fromCore(const bmc::Message &m) {
    Message r;
    r.hash = fromStd(m.hash);
    r.from = fromStd(m.from);
    r.to = fromStd(m.to);
    r.subject = fromStd(m.subject);
    r.body = fromStd(m.body);
    r.folder = fromStd(m.folder);
    r.received = m.received;
    return r;
}
// A secret as text for one call, wiped afterwards.
struct SecretText {
    std::string value;
    explicit SecretText(const QString &text) : value(toStd(text)) {}
    SecretText(const SecretText &) = delete;
    SecretText &operator=(const SecretText &) = delete;
    ~SecretText() {
        sodium_memzero(value.data(), value.size());
    }
};
inline bmc::PublicIdentity toCore(const PublicIdentity &p) {
    return {toStd(p.address),
            toBytes(p.signingKey),
            toBytes(p.encryptionKey),
            p.nonceTrials,
            p.extraBytes,
            p.behaviors,
            p.chan};
}
inline PublicIdentity fromCore(const bmc::PublicIdentity &p) {
    PublicIdentity r;
    r.address = fromStd(p.address);
    r.signingKey = fromBytes(p.signingKey);
    r.encryptionKey = fromBytes(p.encryptionKey);
    r.nonceTrials = p.nonceTrials;
    r.extraBytes = p.extraBytes;
    r.behaviors = p.behaviors;
    r.chan = p.chan;
    return r;
}
inline ChatMessage fromCore(const bmc::ChatMessage &m) {
    ChatMessage r;
    r.id = fromStd(m.id);
    r.localAddress = fromStd(m.localAddress);
    r.peerAddress = fromStd(m.peerAddress);
    r.body = fromStd(m.body);
    r.state = fromStd(m.state);
    r.seqno = m.seqno;
    r.received = m.received;
    r.expires = m.expires;
    r.outgoing = m.outgoing;
    return r;
}
inline ChatSummary fromCore(const bmc::ChatSummary &s) {
    ChatSummary r;
    r.local = fromStd(s.local);
    r.peer = fromStd(s.peer);
    r.body = fromStd(s.body);
    r.state = fromStd(s.state);
    r.received = s.received;
    r.outgoing = s.outgoing;
    r.unread = s.unread;
    return r;
}
inline OutboxItem fromCore(const bmc::OutboxItem &o) {
    OutboxItem r;
    r.id = fromStd(o.id);
    r.kind = fromStd(o.kind);
    r.state = fromStd(o.state);
    r.error = fromStd(o.error);
    r.objectHash = fromStd(o.objectHash);
    r.ackToken = fromBytes(o.ackToken);
    r.ackObject = fromBytes(o.ackObject);
    r.expires = o.expires;
    r.nextAttempt = o.nextAttempt;
    r.attempts = o.attempts;
    r.message = fromCore(o.message);
    r.chat = o.chat;
    return r;
}
inline NetworkJob fromCore(const bmc::NetworkJob &j) {
    NetworkJob r;
    r.id = fromStd(j.id);
    r.owner = fromStd(j.owner);
    r.kind = fromStd(j.kind);
    r.address = fromStd(j.address);
    r.state = fromStd(j.state);
    r.hash = fromStd(j.hash);
    r.error = fromStd(j.error);
    r.payload = fromBytes(j.payload);
    r.expires = j.expires;
    r.nonceTrials = j.nonceTrials;
    r.extraBytes = j.extraBytes;
    return r;
}
inline bmc::NetworkJob toCore(const NetworkJob &j) {
    bmc::NetworkJob r;
    r.id = toStd(j.id);
    r.owner = toStd(j.owner);
    r.kind = toStd(j.kind);
    r.address = toStd(j.address);
    r.state = toStd(j.state);
    r.hash = toStd(j.hash);
    r.error = toStd(j.error);
    r.payload = toBytes(j.payload);
    r.expires = j.expires;
    r.nonceTrials = j.nonceTrials;
    r.extraBytes = j.extraBytes;
    return r;
}
inline StoredPublicKey fromCore(const bmc::StoredPublicKey &k) {
    StoredPublicKey r;
    r.address = fromStd(k.address);
    r.signingKey = fromBytes(k.signingKey);
    r.encryptionKey = fromBytes(k.encryptionKey);
    r.nonceTrials = k.nonceTrials;
    r.extraBytes = k.extraBytes;
    r.behaviors = k.behaviors;
    r.expires = k.expires;
    return r;
}
inline bmc::StoredPublicKey toCore(const StoredPublicKey &k) {
    bmc::StoredPublicKey r;
    r.address = toStd(k.address);
    r.signingKey = toBytes(k.signingKey);
    r.encryptionKey = toBytes(k.encryptionKey);
    r.nonceTrials = k.nonceTrials;
    r.extraBytes = k.extraBytes;
    r.behaviors = k.behaviors;
    r.expires = k.expires;
    return r;
}
inline Subscription fromCore(const bmc::Subscription &s) {
    return Subscription{fromStd(s.address), fromStd(s.label)};
}
inline Contact fromCore(const bmc::Contact &c) {
    return Contact{fromStd(c.address), fromStd(c.label), c.added};
}
inline DeliveryEvent fromCore(const bmc::DeliveryEvent &e) {
    return DeliveryEvent{e.timestamp, fromStd(e.state), fromStd(e.detail)};
}
inline DecodedEnvelope fromCore(bmc::DecodedEnvelope &&e) {
    DecodedEnvelope r;
    r.message = fromCore(e.message);
    r.sender = fromCore(e.sender);
    r.acknowledgment = fromBytes(e.acknowledgment);
    r.broadcast = e.broadcast;
    return r;
}
} // namespace bm::bridge
