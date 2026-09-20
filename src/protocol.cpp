#include "protocol.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
extern "C" {
#include "config.h"
#include "ntb-address.h"
#include "ntb-base58.h"
#include "ntb-ecc.h"
#include "ntb-proto.h"
}
#include <cstring>
#include <stdexcept>
namespace bm {
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
struct Ecc {
    ntb_ecc *p = ntb_ecc_new();
    ~Ecc() {
        ntb_ecc_free(p);
    }
};
struct Buffer {
    ntb_buffer b;
    Buffer() {
        ntb_buffer_init(&b);
    }
    ~Buffer() {
        if (b.data)
            OPENSSL_cleanse(b.data, b.size);
        ntb_buffer_destroy(&b);
    }
};
struct Key {
    EC_KEY *p = nullptr;
    ~Key() {
        EC_KEY_free(p);
    }
};
static QByteArray bytes(const Buffer &b) {
    return QByteArray(reinterpret_cast<const char *>(b.b.data), b.b.length);
}
static const uint8_t *ptr(const QByteArray &b) {
    return reinterpret_cast<const uint8_t *>(b.constData());
}
static ntb_address address(const Identity &i) {
    ntb_address a;
    require(ntb_address_decode(&a, i.address.toUtf8().constData()), "Invalid identity address");
    return a;
}
static QByteArray pub(Ecc &e, const unsigned char *secret) {
    QByteArray b(65, 0);
    ntb_ecc_make_pub_key_bin(e.p, secret, reinterpret_cast<uint8_t *>(b.data()));
    return b;
}
static QString name(const ntb_address &a) {
    char output[NTB_ADDRESS_MAX_LENGTH + 1];
    ntb_address_encode(&a, output);
    return QString::fromLatin1(output);
}
Identity Protocol::fromPrivate(const QString &label, const QByteArray &sign, const QByteArray &enc,
                               int version, int stream, bool chan) {
    require(sign.size() == 32 && enc.size() == 32, "Private keys must have 32 bytes");
    require(version >= 2 && version <= 4 && stream == 1,
            "Only address versions 2–4 in stream 1 are supported");
    Ecc ecc;
    Key s{ntb_ecc_create_key(ecc.p, ptr(sign))}, e{ntb_ecc_create_key(ecc.p, ptr(enc))};
    require(EC_KEY_check_key(s.p) == 1 && EC_KEY_check_key(e.p) == 1, "Invalid private key");
    auto sp = pub(ecc, ptr(sign)), ep = pub(ecc, ptr(enc));
    ntb_address a;
    ntb_address_from_network_keys(&a, version, stream, ptr(sp) + 1, ptr(ep) + 1);
    Identity i;
    i.label = label;
    i.address = name(a);
    i.chan = chan;
    memcpy(i.keys.data(), sign.constData(), 32);
    memcpy(i.keys.data() + 32, enc.constData(), 32);
    return i;
}
Identity Protocol::identity(const QString &label) {
    Ecc ecc;
    Identity i;
    i.label = label;
    for (;;) {
        Key s{ntb_ecc_create_random_key(ecc.p)}, e{ntb_ecc_create_random_key(ecc.p)};
        require(s.p && e.p, "Key generation failed");
        BN_bn2binpad(EC_KEY_get0_private_key(s.p), i.keys.data(), 32);
        BN_bn2binpad(EC_KEY_get0_private_key(e.p), i.keys.data() + 32, 32);
        auto sp = pub(ecc, i.keys.data()), ep = pub(ecc, i.keys.data() + 32);
        ntb_address a;
        ntb_address_from_network_keys(&a, 4, 1, ptr(sp) + 1, ptr(ep) + 1);
        if (a.ripe[0] == 0) {
            i.address = name(a);
            return i;
        }
    }
}
Identity Protocol::channel(const QString &phrase, const QString &label, int version) {
    require(version == 3 || version == 4, "Unsupported channel version");
    require(!phrase.isEmpty(), "Channel phrase cannot be empty");
    auto pass = phrase.toUtf8();
    Ecc ecc;
    Identity i;
    i.label = label;
    i.chan = true;
    for (uint64_t nonce = 0; nonce < 1000000; nonce += 2) {
        Buffer sign, enc;
        ntb_buffer_append(&sign.b, pass.constData(), pass.size());
        ntb_buffer_append(&enc.b, pass.constData(), pass.size());
        ntb_proto_add_var_int(&sign.b, nonce);
        ntb_proto_add_var_int(&enc.b, nonce + 1);
        unsigned char digest[64];
        SHA512(sign.b.data, sign.b.length, digest);
        memcpy(i.keys.data(), digest, 32);
        SHA512(enc.b.data, enc.b.length, digest);
        memcpy(i.keys.data() + 32, digest, 32);
        OPENSSL_cleanse(digest, 64);
        auto sp = pub(ecc, i.keys.data()), ep = pub(ecc, i.keys.data() + 32);
        ntb_address a;
        ntb_address_from_network_keys(&a, version, 1, ptr(sp) + 1, ptr(ep) + 1);
        if (a.ripe[0] == 0) {
            i.address = name(a);
            sodium_memzero(pass.data(), pass.size());
            return i;
        }
    }
    sodium_memzero(pass.data(), pass.size());
    throw std::runtime_error("Channel derivation limit reached");
}
QByteArray Protocol::encodeMessage(const Identity &sender, const Identity &recipient,
                                   const QString &subject, const QString &body, qint64 expires) {
    require(!subject.contains('\n') && !subject.contains('\r'), "Subject must be a single line");
    auto text = ("Subject:" + subject + "\nBody:" + body).toUtf8();
    require(text.size() <= 200000, "Message is too large");
    auto sa = address(sender), ra = address(recipient);
    Ecc ecc;
    auto sp = pub(ecc, sender.keys.data()), ep = pub(ecc, sender.keys.data() + 32);
    Buffer header, plain, result;
    ntb_proto_add_64(&header.b, 0);
    ntb_proto_add_64(&header.b, expires);
    ntb_proto_add_32(&header.b, 2);
    ntb_proto_add_var_int(&header.b, 1);
    ntb_proto_add_var_int(&header.b, ra.stream);
    ntb_proto_add_var_int(&plain.b, sa.version);
    ntb_proto_add_var_int(&plain.b, sa.stream);
    ntb_proto_add_32(&plain.b, 0);
    ntb_buffer_append(&plain.b, ptr(sp) + 1, 64);
    ntb_buffer_append(&plain.b, ptr(ep) + 1, 64);
    if (sa.version >= 3) {
        ntb_proto_add_var_int(&plain.b, 1000);
        ntb_proto_add_var_int(&plain.b, 1000);
    }
    ntb_buffer_append(&plain.b, ra.ripe, 20);
    ntb_proto_add_var_int(&plain.b, 2);
    ntb_proto_add_var_int(&plain.b, text.size());
    ntb_buffer_append(&plain.b, text.constData(), text.size());
    ntb_proto_add_var_int(&plain.b, 0);
    auto signedData = bytes(header).mid(8) + bytes(plain);
    auto digest = QCryptographicHash::hash(signedData, QCryptographicHash::Sha256);
    Key key{ntb_ecc_create_key(ecc.p, sender.keys.data())};
    unsigned char sig[80];
    unsigned int size = 80;
    require(ECDSA_sign(0, ptr(digest), digest.size(), sig, &size, key.p) == 1,
            "Message signing failed");
    ntb_proto_add_var_int(&plain.b, size);
    ntb_buffer_append(&plain.b, sig, size);
    ntb_buffer_append(&result.b, header.b.data, header.b.length);
    auto point = ntb_ecc_make_pub_key_point(ecc.p, recipient.keys.data() + 32);
    ntb_ecc_encrypt_with_point(ecc.p, point, plain.b.data, plain.b.length, &result.b);
    EC_POINT_free(point);
    sodium_memzero(text.data(), text.size());
    sodium_memzero(signedData.data(), signedData.size());
    return bytes(result);
}
std::optional<Message> Protocol::decodeMessage(const QByteArray &object,
                                               const Identity &recipient) {
    if (object.size() > 262144)
        return {};
    ntb_proto_object_header h;
    auto hs = ntb_proto_get_object_header(ptr(object), object.size(), &h);
    if (hs < 0 || h.type != 2 || h.version != 1 || h.stream != 1)
        return {};
    // Bound both EC coordinates before entering legacy ECIES parsing.
    const auto encrypted = object.mid(hs);
    if (encrypted.size() < 16 + 2 + 2 + 32 + 2 + 32 + 16 + 32)
        return {};
    size_t offset = 18;
    for (int j = 0; j < 2; ++j) {
        if (offset + 2 > size_t(encrypted.size()))
            return {};
        auto length = ntb_proto_get_16(ptr(encrypted) + offset);
        offset += 2;
        if (length == 0 || length > 32 || offset + length > size_t(encrypted.size()))
            return {};
        offset += length;
    }
    Ecc ecc;
    Key key{ntb_ecc_create_key(ecc.p, recipient.keys.data() + 32)};
    Buffer plain;
    if (!ntb_ecc_decrypt(ecc.p, key.p, ptr(encrypted), encrypted.size(), &plain.b))
        return {};
    ntb_proto_decrypted_msg m;
    if (!ntb_proto_get_decrypted_msg(plain.b.data, plain.b.length, &m))
        return {};
    auto ra = address(recipient);
    if (CRYPTO_memcmp(ra.ripe, m.destination_ripe, 20) != 0)
        return {};
    if (m.sender_address_version < 2 || m.sender_address_version > 4 ||
        m.sender_stream_number != 1 || m.message_length > 200000 || m.sig_length > 80)
        return {};
    QByteArray publicKey(65, 0);
    publicKey[0] = 4;
    memcpy(publicKey.data() + 1, m.sender_signing_key, 64);
    Key signing{EC_KEY_new_by_curve_name(NID_secp256k1)};
    const unsigned char *p = ptr(publicKey);
    if (!o2i_ECPublicKey(&signing.p, &p, 65) || EC_KEY_check_key(signing.p) != 1)
        return {};
    QCryptographicHash sha(QCryptographicHash::Sha256);
    sha.addData(QByteArrayView(object.constData() + 8, hs - 8));
    sha.addData(QByteArrayView(reinterpret_cast<const char *>(plain.b.data), m.signed_data_length));
    auto digest = sha.result();
    bool valid = ECDSA_verify(0, ptr(digest), digest.size(), m.sig, m.sig_length, signing.p) == 1;
    if (!valid) {
        QCryptographicHash old(QCryptographicHash::Sha1);
        old.addData(QByteArrayView(object.constData() + 8, hs - 8));
        old.addData(
            QByteArrayView(reinterpret_cast<const char *>(plain.b.data), m.signed_data_length));
        digest = old.result();
        valid = ECDSA_verify(0, ptr(digest), digest.size(), m.sig, m.sig_length, signing.p) == 1;
    }
    if (!valid)
        return {};
    ntb_address sender;
    ntb_address_from_network_keys(&sender, m.sender_address_version, m.sender_stream_number,
                                  m.sender_signing_key, m.sender_encryption_key);
    Message result;
    result.hash = inventoryHash(object);
    result.from = name(sender);
    result.to = recipient.address;
    result.folder = recipient.chan ? "Channels" : "Inbox";
    result.received = QDateTime::currentSecsSinceEpoch();
    if (m.encoding != 1 && m.encoding != 2)
        return {};
    auto text = QString::fromUtf8(reinterpret_cast<const char *>(m.message), m.message_length);
    if (m.encoding == 2 && text.startsWith("Subject:")) {
        auto split = text.indexOf("\nBody:");
        if (split >= 0) {
            result.subject = text.mid(8, split - 8);
            result.body = text.mid(split + 6);
        } else
            result.body = text;
    } else
        result.body = text;
    return result;
}
QString Protocol::inventoryHash(const QByteArray &b) {
    return QCryptographicHash::hash(QCryptographicHash::hash(b, QCryptographicHash::Sha512),
                                    QCryptographicHash::Sha512)
        .left(32)
        .toHex();
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
    for (auto &i : imported)
        identities_.push_back(std::move(i));
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
    auto channel = Protocol::channel(phrase, label, version);
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
