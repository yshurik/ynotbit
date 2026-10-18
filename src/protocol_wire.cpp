#include "protocol_wire.h"
#include "qt_bridge.h"
#include <bmc/wire.h>

namespace bm {
static_assert(kMaxObjectBytes == bmc::kMaxObjectBytes && kMaxLetterText == bmc::kMaxLetterText);

std::optional<ObjectHeader> Wire::header(const QByteArray &object) {
    const auto h = bmc::Wire::header(bridge::view(object));
    if (!h)
        return {};
    return ObjectHeader{h->type, h->version, h->stream, h->expires, h->headerSize};
}
bool Wire::validAddress(const QString &s) {
    return bmc::Wire::validAddress(bridge::toStd(s));
}
PublicIdentity Wire::publicIdentity(const Identity &i) {
    return bridge::fromCore(bmc::Wire::publicIdentity(bridge::CoreIdentity(i)));
}
QByteArray Wire::encodeMessage(const Identity &sender, const PublicIdentity &recipient,
                               const QString &subject, const QString &body, qint64 expires,
                               const QByteArray &ackObject) {
    return bridge::fromBytes(bmc::Wire::encodeMessage(
        bridge::CoreIdentity(sender), bridge::toCore(recipient), bridge::toStd(subject),
        bridge::toStd(body), expires, bridge::view(ackObject)));
}
std::optional<DecodedEnvelope> Wire::decodeMessage(const QByteArray &object,
                                                   const Identity &recipient) {
    auto e = bmc::Wire::decodeMessage(bridge::view(object), bridge::CoreIdentity(recipient));
    if (!e)
        return {};
    return bridge::fromCore(std::move(*e));
}
QByteArray Wire::encodePubkey(const Identity &i, qint64 expires) {
    return bridge::fromBytes(bmc::Wire::encodePubkey(bridge::CoreIdentity(i), expires));
}
std::optional<PublicIdentity> Wire::decodePubkey(const QByteArray &object,
                                                 const QString &requested) {
    const auto p = bmc::Wire::decodePubkey(bridge::view(object), bridge::toStd(requested));
    if (!p)
        return {};
    return bridge::fromCore(*p);
}
QByteArray Wire::getPubkey(const QString &requested, qint64 expires) {
    return bridge::fromBytes(bmc::Wire::getPubkey(bridge::toStd(requested), expires));
}
bool Wire::requestsIdentity(const QByteArray &object, const Identity &i) {
    return bmc::Wire::requestsIdentity(bridge::view(object), bridge::CoreIdentity(i));
}
QByteArray Wire::acknowledgment(const QByteArray &token, qint64 expires) {
    return bridge::fromBytes(bmc::Wire::acknowledgment(bridge::view(token), expires));
}
QByteArray Wire::acknowledgmentToken(const QByteArray &object) {
    return bridge::fromBytes(bmc::Wire::acknowledgmentToken(bridge::view(object)));
}
QByteArray Wire::encodeBroadcast(const Identity &i, const QString &subject, const QString &body,
                                 qint64 expires) {
    return bridge::fromBytes(bmc::Wire::encodeBroadcast(
        bridge::CoreIdentity(i), bridge::toStd(subject), bridge::toStd(body), expires));
}
std::optional<DecodedEnvelope> Wire::decodeBroadcast(const QByteArray &object,
                                                     const QString &address) {
    auto e = bmc::Wire::decodeBroadcast(bridge::view(object), bridge::toStd(address));
    if (!e)
        return {};
    return bridge::fromCore(std::move(*e));
}
QByteArray Wire::frame(const QByteArray &object) {
    return bridge::fromBytes(bmc::Wire::frame(bridge::view(object)));
}
} // namespace bm
