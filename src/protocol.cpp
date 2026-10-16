#include "protocol.h"
#include "qt_bridge.h"
#include <QByteArrayView>
#include <QFile>
#include <bmc/protocol.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <sodium.h>
#include <stdexcept>
extern "C" {
#include "config.h"
#include "ntb-address.h"
#include "ntb-base58.h"
}
#include <cstring>
namespace bm {
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
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

struct WipedBytes {
    QByteArray bytes;
    ~WipedBytes() {
        if (!bytes.isEmpty())
            sodium_memzero(bytes.data(), bytes.size());
    }
};
static Secret wif(QByteArrayView text) {
    require(text.size() <= 128, "Invalid private key encoding");
    Secret decoded(128);
    auto n = ntb_base58_decode(text.data(), text.size(), decoded.data(), decoded.size());
    require(n == 37 && decoded.data()[0] == 128, "Invalid private key encoding");
    unsigned char first[32], second[32];
    SHA256(decoded.data(), 33, first);
    SHA256(first, 32, second);
    require(CRYPTO_memcmp(decoded.data() + 33, second, 4) == 0, "Private key checksum mismatch");
    Secret key(32);
    memcpy(key.data(), decoded.data() + 1, 32);
    return key;
}
void Vault::importKeys(const QString &path) {
    require(unlocked(), "Vault is locked");
    QFile f(path);
    require(f.open(QIODevice::ReadOnly) && f.size() <= 1024 * 1024, "Cannot read keys file");
    WipedBytes content{f.readAll()};
    QByteArrayView remaining(content.bytes);
    QString section, label;
    bool chan = false;
    Secret signing, encryption;
    std::vector<Identity> imported;
    auto flush = [&] {
        if (!section.startsWith("BM-") || signing.size() != 32 || encryption.size() != 32)
            return;
        ntb_address a;
        require(ntb_address_decode(&a, section.toLatin1().constData()), "Invalid imported address");
        auto signView = QByteArray::fromRawData(reinterpret_cast<const char *>(signing.data()), 32);
        auto encView =
            QByteArray::fromRawData(reinterpret_cast<const char *>(encryption.data()), 32);
        auto identity = Protocol::fromPrivate(label.isEmpty() ? section : label, signView, encView,
                                              a.version, a.stream, chan);
        require(identity.address == section, "Imported keys do not match their address");
        for (const auto &existing : identities_)
            if (existing.address == identity.address)
                return;
        for (const auto &existing : imported)
            if (existing.address == identity.address)
                return;
        imported.push_back(std::move(identity));
    };
    while (!remaining.empty()) {
        auto end = remaining.indexOf('\n');
        auto line = (end < 0 ? remaining : remaining.first(end)).trimmed();
        remaining = end < 0 ? QByteArrayView() : remaining.sliced(end + 1);
        if (line.startsWith('[') && line.endsWith(']')) {
            flush();
            section = QString::fromUtf8(line.sliced(1, line.size() - 2));
            label.clear();
            chan = false;
            signing.clear();
            encryption.clear();
        } else {
            auto equal = line.indexOf('=');
            if (equal < 1)
                continue;
            auto field = line.first(equal).trimmed(), value = line.sliced(equal + 1).trimmed();
            if (field == "privsigningkey")
                signing = wif(value);
            else if (field == "privencryptionkey")
                encryption = wif(value);
            else if (field == "label")
                label = QString::fromUtf8(value);
            else if (field == "chan")
                chan = value == "true";
        }
    }
    flush();
    require(!imported.empty(), "No new private identities found in this file");
    auto count = identities_.size();
    for (auto &i : imported) {
        if (i.chan)
            i.label = channelName(i.label);
        identities_.push_back(std::move(i));
    }
    try {
        save();
    } catch (...) {
        identities_.resize(count);
        throw;
    }
}
} // namespace bm

namespace bm {
QString Vault::addChannel(const QString &phrase, const QString &label,
                          const QString &expectedAddress) {
    require(unlocked_, "Vault is locked");
    int version = 4;
    if (!expectedAddress.isEmpty()) {
        ntb_address a;
        require(ntb_address_decode(&a, expectedAddress.toLatin1().constData()) && a.stream == 1,
                "Invalid expected chan address");
        version = a.version;
    }
    // An empty label names the chan after its phrase.
    auto channel =
        Protocol::channel(phrase, channelName(label.isEmpty() ? phrase : label), version);
    require(expectedAddress.isEmpty() || channel.address == expectedAddress,
            "The shared phrase does not match that chan address");
    for (const auto &i : identities_)
        require(i.address != channel.address, "Channel already joined");
    identities_.push_back(std::move(channel));
    if (identities_.size() == 1)
        identities_.back().isDefault = true;
    try {
        save();
    } catch (...) {
        identities_.pop_back();
        throw;
    }
    return identities_.back().address;
}
} // namespace bm
