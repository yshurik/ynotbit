#pragma once
#include "storage.h"
#include <optional>
namespace bm {
struct PublicIdentity {
    QString address;
    QByteArray signingKey, encryptionKey;
    quint64 nonceTrials = 1000, extraBytes = 1000;
    quint32 behaviors = 1;
    bool chan = false;
};
struct ObjectHeader {
    quint32 type;
    quint64 version, stream;
    qint64 expires;
    int headerSize;
};
struct DecodedEnvelope {
    Message message;
    PublicIdentity sender;
    QByteArray acknowledgment;
    bool broadcast = false;
};
class Wire {
  public:
    static std::optional<ObjectHeader> header(const QByteArray &);
    static bool validAddress(const QString &);
    static PublicIdentity publicIdentity(const Identity &);
    static QByteArray encodeMessage(const Identity &, const PublicIdentity &,
                                    const QString &subject, const QString &body, qint64 expires,
                                    const QByteArray &ackObject = {});
    static std::optional<DecodedEnvelope> decodeMessage(const QByteArray &, const Identity &);
    static QByteArray encodePubkey(const Identity &, qint64 expires);
    static std::optional<PublicIdentity> decodePubkey(const QByteArray &, const QString &address);
    static QByteArray getPubkey(const QString &address, qint64 expires);
    static bool requestsIdentity(const QByteArray &, const Identity &);
    static QByteArray acknowledgment(const QByteArray &token, qint64 expires);
    static QByteArray acknowledgmentToken(const QByteArray &object);
    static QByteArray encodeBroadcast(const Identity &, const QString &subject, const QString &body,
                                      qint64 expires);
    static std::optional<DecodedEnvelope> decodeBroadcast(const QByteArray &,
                                                          const QString &address);
    static QByteArray frame(const QByteArray &object);
};
} // namespace bm
