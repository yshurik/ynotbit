#include "protocol.h"
#include "qt_bridge.h"
#include <bmc/protocol.h>
namespace bm {
Identity Protocol::fromPrivate(const QString &label, const QByteArray &sign, const QByteArray &enc,
                               int version, int stream, bool chan) {
    return bridge::fromCore(bmc::Protocol::fromPrivate(bridge::toStd(label), bridge::view(sign),
                                                       bridge::view(enc), version, stream, chan));
}
Identity Protocol::identity(const QString &label) {
    return bridge::fromCore(bmc::Protocol::identity(bridge::toStd(label)));
}
Identity Protocol::channel(const QString &phrase, const QString &label, int version) {
    bridge::SecretText pass(phrase);
    return bridge::fromCore(bmc::Protocol::channel(pass.value, bridge::toStd(label), version));
}
QByteArray Protocol::encodeMessage(const Identity &sender, const Identity &recipient,
                                   const QString &subject, const QString &body, qint64 expires) {
    return bridge::fromBytes(
        bmc::Protocol::encodeMessage(bridge::CoreIdentity(sender), bridge::CoreIdentity(recipient),
                                     bridge::toStd(subject), bridge::toStd(body), expires));
}
std::optional<Message> Protocol::decodeMessage(const QByteArray &object,
                                               const Identity &recipient) {
    const auto message =
        bmc::Protocol::decodeMessage(bridge::view(object), bridge::CoreIdentity(recipient));
    if (!message)
        return {};
    return bridge::fromCore(*message);
}
QString Protocol::inventoryHash(const QByteArray &object) {
    return bridge::fromStd(bmc::Protocol::inventoryHash(bridge::view(object)));
}
} // namespace bm
