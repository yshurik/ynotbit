// The Qt wrappers in src/ call the core (bmclient, which has no Qt) through these conversions.
#pragma once
#include "storage.h"
#include <QByteArray>
#include <QString>
#include <bmc/bytes.h>
#include <bmc/identity.h>
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
} // namespace bm::bridge
