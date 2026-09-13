#include "scanner.h"
#include "protocol.h"
#include <QCryptographicHash>
#include <QFile>
namespace bm {
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit) {
    if (!vault.unlocked() || !mailbox.isOpen())
        return 0;
    QStringList addresses;
    for (const auto &identity : vault.identities())
        addresses << identity.address;
    addresses.sort();
    mailbox.bindIdentities(QString::fromLatin1(
        QCryptographicHash::hash(addresses.join('\n').toUtf8(), QCryptographicHash::Sha256)
            .toHex()));
    int decodedCount = 0;
    for (const auto &o : cache.after(mailbox.checkpoint(), limit)) {
        QFile f(o.path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (!QFile::exists(o.path)) {
                mailbox.advance(o.sequence);
                continue;
            }
            throw std::runtime_error("A retained object cannot be read; scan checkpoint preserved");
        }
        if (f.size() > 262144)
            throw std::runtime_error("Cached object exceeds protocol limit");
        auto data = f.readAll();
        bool found = false;
        for (const auto &i : vault.identities()) {
            auto decoded = Protocol::decodeMessage(data, i);
            if (decoded) {
                mailbox.store(o.hash, decoded->from, decoded->to, decoded->subject, decoded->body,
                              o.sequence, decoded->folder);
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
