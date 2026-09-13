#pragma once
#include "storage.h"
#include <optional>
namespace bm {
class Protocol {
  public:
    static Identity identity(const QString &label);
    static Identity channel(const QString &phrase, const QString &label, int version = 4);
    static Identity fromPrivate(const QString &label, const QByteArray &signing,
                                const QByteArray &encryption, int version = 4, int stream = 1,
                                bool chan = false);
    static QByteArray encodeMessage(const Identity &sender, const Identity &recipient,
                                    const QString &subject, const QString &body, qint64 expires);
    static std::optional<Message> decodeMessage(const QByteArray &, const Identity &recipient);
    static QString inventoryHash(const QByteArray &);
};
} // namespace bm
