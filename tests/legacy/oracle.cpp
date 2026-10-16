#include "oracle.h"
#include "key_search.h"
#include "pow.h"
#include "protocol.h"
#include "protocol_wire.h"
#include <cstring>

namespace oracle {
namespace {
QString q(const std::string &s) {
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}
std::string s(const QString &text) {
    return text.toUtf8().toStdString();
}
QByteArray qb(const Bytes &b) {
    return QByteArray(reinterpret_cast<const char *>(b.data()), qsizetype(b.size()));
}
Bytes bytes(const QByteArray &a) {
    const auto *p = reinterpret_cast<const uint8_t *>(a.constData());
    return Bytes(p, p + a.size());
}
legacy::Identity toLegacy(const Id &id) {
    legacy::Identity i;
    i.label = q(id.label);
    i.address = q(id.address);
    i.chan = id.chan;
    std::memcpy(i.keys.data(), id.keys.data(), 64);
    return i;
}
Id fromLegacy(const legacy::Identity &i) {
    Id id;
    id.label = s(i.label);
    id.address = s(i.address);
    id.chan = i.chan;
    std::memcpy(id.keys.data(), i.keys.data(), 64);
    return id;
}
legacy::PublicIdentity toLegacy(const Pub &p) {
    legacy::PublicIdentity r;
    r.address = q(p.address);
    r.signingKey = qb(p.signingKey);
    r.encryptionKey = qb(p.encryptionKey);
    r.nonceTrials = p.nonceTrials;
    r.extraBytes = p.extraBytes;
    r.behaviors = p.behaviors;
    r.chan = p.chan;
    return r;
}
Pub fromLegacy(const legacy::PublicIdentity &p) {
    Pub r;
    r.address = s(p.address);
    r.signingKey = bytes(p.signingKey);
    r.encryptionKey = bytes(p.encryptionKey);
    r.nonceTrials = p.nonceTrials;
    r.extraBytes = p.extraBytes;
    r.behaviors = p.behaviors;
    r.chan = p.chan;
    return r;
}
Msg fromLegacy(const legacy::Message &m) {
    return {s(m.hash), s(m.from), s(m.to), s(m.subject), s(m.body), s(m.folder)};
}
Env fromLegacy(const legacy::DecodedEnvelope &e) {
    return {fromLegacy(e.message), fromLegacy(e.sender), bytes(e.acknowledgment), e.broadcast};
}
std::optional<Env> fromLegacy(const std::optional<legacy::DecodedEnvelope> &e) {
    if (!e)
        return std::nullopt;
    return fromLegacy(*e);
}
} // namespace

Id fromPrivate(const std::string &label, const Bytes &signing, const Bytes &encryption, int version,
               int stream, bool chan) {
    return fromLegacy(legacy::Protocol::fromPrivate(q(label), qb(signing), qb(encryption), version,
                                                    stream, chan));
}
Id channel(const std::string &phrase, const std::string &label, int version) {
    return fromLegacy(legacy::Protocol::channel(q(phrase), q(label), version));
}
Bytes protocolEncode(const Id &sender, const Id &recipient, const std::string &subject,
                     const std::string &body, int64_t expires) {
    return bytes(legacy::Protocol::encodeMessage(toLegacy(sender), toLegacy(recipient), q(subject),
                                                 q(body), expires));
}
std::optional<Msg> protocolDecode(const Bytes &object, const Id &recipient) {
    auto m = legacy::Protocol::decodeMessage(qb(object), toLegacy(recipient));
    if (!m)
        return std::nullopt;
    return fromLegacy(*m);
}
std::string inventoryHash(const Bytes &object) {
    return s(legacy::Protocol::inventoryHash(qb(object)));
}
std::optional<Header> header(const Bytes &object) {
    auto h = legacy::Wire::header(qb(object));
    if (!h)
        return std::nullopt;
    return Header{h->type, h->version, h->stream, h->expires, h->headerSize};
}
bool validAddress(const std::string &address) {
    return legacy::Wire::validAddress(q(address));
}
Pub publicIdentity(const Id &identity) {
    return fromLegacy(legacy::Wire::publicIdentity(toLegacy(identity)));
}
Bytes wireEncode(const Id &sender, const Pub &recipient, const std::string &subject,
                 const std::string &body, int64_t expires, const Bytes &ackObject) {
    return bytes(legacy::Wire::encodeMessage(toLegacy(sender), toLegacy(recipient), q(subject),
                                             q(body), expires, qb(ackObject)));
}
std::optional<Env> wireDecode(const Bytes &object, const Id &recipient) {
    return fromLegacy(legacy::Wire::decodeMessage(qb(object), toLegacy(recipient)));
}
Bytes encodePubkey(const Id &identity, int64_t expires) {
    return bytes(legacy::Wire::encodePubkey(toLegacy(identity), expires));
}
std::optional<Pub> decodePubkey(const Bytes &object, const std::string &address) {
    auto p = legacy::Wire::decodePubkey(qb(object), q(address));
    if (!p)
        return std::nullopt;
    return fromLegacy(*p);
}
Bytes getPubkey(const std::string &address, int64_t expires) {
    return bytes(legacy::Wire::getPubkey(q(address), expires));
}
bool requestsIdentity(const Bytes &object, const Id &identity) {
    return legacy::Wire::requestsIdentity(qb(object), toLegacy(identity));
}
Bytes acknowledgment(const Bytes &token, int64_t expires) {
    return bytes(legacy::Wire::acknowledgment(qb(token), expires));
}
Bytes acknowledgmentToken(const Bytes &object) {
    return bytes(legacy::Wire::acknowledgmentToken(qb(object)));
}
Bytes encodeBroadcast(const Id &identity, const std::string &subject, const std::string &body,
                      int64_t expires) {
    return bytes(legacy::Wire::encodeBroadcast(toLegacy(identity), q(subject), q(body), expires));
}
std::optional<Env> decodeBroadcast(const Bytes &object, const std::string &address) {
    return fromLegacy(legacy::Wire::decodeBroadcast(qb(object), q(address)));
}
Bytes frame(const Bytes &object) {
    return bytes(legacy::Wire::frame(qb(object)));
}
bool powValid(const Bytes &object, int64_t now, uint64_t trials, uint64_t extra) {
    return legacy::ProofOfWork::valid(qb(object), now, trials, extra);
}
uint64_t trialValue(uint64_t nonce, const std::array<uint8_t, 64> &initial) {
    return legacy::ProofOfWork::trialValue(nonce, initial.data());
}
Id phraseIdentity(const std::string &phrase, const std::string &label, uint64_t pair) {
    return fromLegacy(legacy::phraseIdentity(q(phrase), q(label), pair));
}
std::optional<uint64_t> firstPair(const std::string &phrase, int zeroBytes, uint64_t limit) {
    return legacy::firstPair(q(phrase), zeroBytes, limit);
}
int zeroBytes(const std::string &address) {
    return legacy::zeroBytes(q(address));
}
} // namespace oracle
