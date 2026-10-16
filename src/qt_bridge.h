// The Qt wrappers in src/ call the core (bmclient, which has no Qt) through these conversions.
#pragma once
#include <QByteArray>
#include <QString>
#include <bmc/bytes.h>

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
} // namespace bm::bridge
