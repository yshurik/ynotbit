#include "scanner.h"
#include "ntb-object-db.h"
#include "protocol.h"
#include "protocol_wire.h"
#include <QCryptographicHash>
namespace bm {
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit) {
    if (!vault.unlocked() || !mailbox.isOpen())
        return 0;
    QStringList addresses;
    const auto identities = vault.identities();
    for (const auto &identity : identities)
        addresses << identity.address;
    addresses.sort();
    mailbox.bindIdentities(QString::fromLatin1(
        QCryptographicHash::hash(addresses.join('\n').toUtf8(), QCryptographicHash::Sha256)
            .toHex()));
    int decodedCount = 0;
    for (const auto &o : cache.after(mailbox.checkpoint(), limit)) {
        if (o.payload.size() > NTB_OBJECT_DB_MAX_OBJECT_SIZE) {
            mailbox.advance(o.sequence);
            continue;
        }
        const auto data = o.payload;
        bool found = false;
        for (const auto &i : identities) {
            auto decoded = Protocol::decodeMessage(data, i);
            if (decoded) {
                const auto header = Wire::header(data);
                if (header)
                    mailbox.storeRouted(o.hash, decoded->from, decoded->to, decoded->subject,
                                        decoded->body, o.sequence, decoded->folder, header->expires);
                else
                    mailbox.advance(o.sequence);
                found = true;
                ++decodedCount;
                break;
            }
        }
        if (!found)
            mailbox.advance(o.sequence);
    }
    return decodedCount;
}
} // namespace bm
