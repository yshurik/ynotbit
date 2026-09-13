#include "protocol_wire.h"
#include "protocol.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <limits>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>
extern "C" {
#include "config.h"
#include "ntb-address.h"
}
namespace bm {
namespace {
constexpr int MaxObject = 262144, MaxText = 200000;
void check(bool ok, const char *what = "Invalid wire object") {
    if (!ok)
        throw std::runtime_error(what);
}
const unsigned char *ptr(const QByteArray &b) {
    return reinterpret_cast<const unsigned char *>(b.constData());
}
unsigned char *out(QByteArray &b) {
    return reinterpret_cast<unsigned char *>(b.data());
}
struct Wiped {
    QByteArray b;
    ~Wiped() {
        if (!b.isEmpty())
            sodium_memzero(b.data(), b.size());
    }
};
using Key = std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)>;
using Bn = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
using Point = std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)>;
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
struct Reader {
    const QByteArray &b;
    int pos = 0;
    QByteArray take(quint64 n) {
        check(n <= quint64(b.size() - pos));
        auto v = b.mid(pos, qsizetype(n));
        pos += int(n);
        return v;
    }
    quint64 number(int n) {
        auto v = take(n);
        quint64 r = 0;
        for (auto c : v)
            r = (r << 8) | static_cast<unsigned char>(c);
        return r;
    }
    quint64 var() {
        auto n = number(1);
        if (n < 253)
            return n;
        int size = n == 253 ? 2 : n == 254 ? 4 : 8;
        n = number(size);
        check(n >= (size == 2 ? 253ULL : size == 4 ? 65536ULL : 4294967296ULL));
        return n;
    }
    QByteArray blob(quint64 max) {
        auto n = var();
        check(n <= max);
        return take(n);
    }
    void end() {
        check(pos == b.size());
    }
};
void number(QByteArray &b, quint64 v, int n) {
    for (int i = n - 1; i >= 0; --i)
        b.append(char(v >> (i * 8)));
}
void var(QByteArray &b, quint64 v) {
    if (v < 253)
        b.append(char(v));
    else {
        int n = v <= 65535 ? 2 : v <= 0xffffffffULL ? 4 : 8;
        b.append(char(n == 2 ? 253 : n == 4 ? 254 : 255));
        number(b, v, n);
    }
}
void blob(QByteArray &b, const QByteArray &v) {
    var(b, v.size());
    b += v;
}
QByteArray hash(const QByteArray &b, QCryptographicHash::Algorithm a = QCryptographicHash::Sha512) {
    return QCryptographicHash::hash(b, a);
}
QString name(const ntb_address &a) {
    char buf[NTB_ADDRESS_MAX_LENGTH + 1];
    ntb_address_encode(&a, buf);
    return QString::fromLatin1(buf);
}
ntb_address address(const QString &s) {
    ntb_address a{};
    check(s.size() <= 64 && s.startsWith("BM-") && !s.contains(QChar(0)) &&
              ntb_address_decode(&a, s.toLatin1().constData()) && a.version >= 2 &&
              a.version <= 4 && a.stream == 1 && name(a) == s,
          "Invalid Bitmessage address");
    return a;
}
Key privateKey(const unsigned char *secret) {
    Key k(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    check(bool(k));
    Bn n(BN_bin2bn(secret, 32, nullptr), BN_clear_free);
    Bn order(BN_new(), BN_clear_free);
    auto group = EC_KEY_get0_group(k.get());
    check(n && order && EC_GROUP_get_order(group, order.get(), nullptr) == 1 &&
              !BN_is_zero(n.get()) && BN_cmp(n.get(), order.get()) < 0,
          "Invalid private scalar");
    Point p(EC_POINT_new(group), EC_POINT_free);
    check(p && EC_POINT_mul(group, p.get(), n.get(), nullptr, nullptr, nullptr) == 1 &&
          EC_KEY_set_private_key(k.get(), n.get()) == 1 &&
          EC_KEY_set_public_key(k.get(), p.get()) == 1);
    return k;
}
Key publicKey(const QByteArray &bytes) {
    check(bytes.size() == 65 && bytes[0] == 4, "Invalid public key");
    Key k(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    check(bool(k));
    Point p(EC_POINT_new(EC_KEY_get0_group(k.get())), EC_POINT_free);
    check(p &&
              EC_POINT_oct2point(EC_KEY_get0_group(k.get()), p.get(), ptr(bytes), 65, nullptr) ==
                  1 &&
              EC_POINT_is_at_infinity(EC_KEY_get0_group(k.get()), p.get()) == 0 &&
              EC_KEY_set_public_key(k.get(), p.get()) == 1 && EC_KEY_check_key(k.get()) == 1,
          "Invalid public point");
    return k;
}
QByteArray publicBytes(EC_KEY *k) {
    QByteArray b(65, 0);
    check(EC_POINT_point2oct(EC_KEY_get0_group(k), EC_KEY_get0_public_key(k),
                             POINT_CONVERSION_UNCOMPRESSED, out(b), 65, nullptr) == 65);
    return b;
}
ntb_address boundAddress(const PublicIdentity &i) {
    auto a = address(i.address);
    auto s = publicKey(i.signingKey), e = publicKey(i.encryptionKey);
    ntb_address derived{};
    ntb_address_from_network_keys(&derived, a.version, a.stream, ptr(i.signingKey) + 1,
                                  ptr(i.encryptionKey) + 1);
    check(ntb_address_equal(&derived, &a), "Public keys do not match address");
    return a;
}
QByteArray base(quint32 type, quint64 version, qint64 expires) {
    check(expires > 0, "Invalid expiry");
    QByteArray b(8, 0);
    number(b, expires, 8);
    number(b, type, 4);
    var(b, version);
    var(b, 1);
    return b;
}
ObjectHeader parseHeader(Reader &r) {
    check(r.b.size() <= MaxObject);
    r.number(8);
    auto expiry = r.number(8);
    check(expiry > 0 && expiry <= quint64(std::numeric_limits<qint64>::max()));
    auto type = quint32(r.number(4));
    auto version = r.var(), stream = r.var();
    check(stream == 1);
    return {type, version, stream, qint64(expiry), r.pos};
}
QByteArray signature(const Identity &i, const QByteArray &data) {
    auto k = privateKey(i.keys.data());
    auto digest = hash(data, QCryptographicHash::Sha256);
    QByteArray sig(ECDSA_size(k.get()), 0);
    unsigned int n = sig.size();
    check(ECDSA_sign(0, ptr(digest), digest.size(), out(sig), &n, k.get()) == 1, "Signing failed");
    sig.resize(n);
    return sig;
}
void verify(const QByteArray &key, const QByteArray &data, Reader &r) {
    auto sig = r.blob(80);
    r.end();
    auto k = publicKey(key);
    for (auto algo : {QCryptographicHash::Sha256, QCryptographicHash::Sha1}) {
        auto d = hash(data, algo);
        if (ECDSA_verify(0, ptr(d), d.size(), ptr(sig), sig.size(), k.get()) == 1)
            return;
    }
    check(false, "Signature mismatch");
}
// ECIES as used by pyelliptic: IV, curve 714, sized X/Y, AES-CBC, HMAC-SHA256.
// No legacy parser assertions are reachable with network-controlled data.
void shared(EC_KEY *priv, EC_KEY *pub, Secret &keys) {
    Secret raw(32);
    check(ECDH_compute_key(raw.data(), 32, EC_KEY_get0_public_key(pub), priv, nullptr) == 32,
          "ECDH failed");
    check(SHA512(raw.data(), 32, keys.data()) != nullptr);
}
QByteArray mac(const QByteArray &data, const Secret &keys) {
    QByteArray result(32, 0);
    unsigned int n = 32;
    check(HMAC(EVP_sha256(), keys.data() + 32, 32, ptr(data), data.size(), out(result), &n) &&
          n == 32);
    return result;
}
QByteArray encrypt(const QByteArray &plain, const QByteArray &recipient) {
    auto pub = publicKey(recipient);
    Key ephemeral(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    check(ephemeral && EC_KEY_generate_key(ephemeral.get()) == 1);
    Secret keys(64);
    shared(ephemeral.get(), pub.get(), keys);
    QByteArray result(16, 0);
    check(RAND_bytes(out(result), 16) == 1);
    auto p = publicBytes(ephemeral.get());
    number(result, NID_secp256k1, 2);
    number(result, 32, 2);
    result += p.mid(1, 32);
    number(result, 32, 2);
    result += p.mid(33, 32);
    Cipher c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    check(bool(c));
    check(EVP_EncryptInit_ex(c.get(), EVP_aes_256_cbc(), nullptr, keys.data(), ptr(result)) == 1);
    QByteArray encrypted(plain.size() + 16, 0);
    int n = 0, tail = 0;
    check(EVP_EncryptUpdate(c.get(), out(encrypted), &n, ptr(plain), plain.size()) == 1 &&
          EVP_EncryptFinal_ex(c.get(), out(encrypted) + n, &tail) == 1);
    encrypted.resize(n + tail);
    result += encrypted;
    result += mac(result, keys);
    return result;
}
QByteArray decrypt(const QByteArray &data, const unsigned char *secret) {
    check(data.size() <= MaxObject);
    Reader r{data};
    auto iv = r.take(16);
    check(r.number(2) == NID_secp256k1);
    QByteArray pub(1, 4);
    for (int j = 0; j < 2; ++j) {
        auto n = r.number(2);
        check(n > 0 && n <= 32);
        pub += QByteArray(32 - n, 0);
        pub += r.take(n);
    }
    int size = data.size() - r.pos - 32;
    check(size >= 16 && size % 16 == 0);
    auto ciphertext = r.take(size), tag = r.take(32);
    r.end();
    auto k = privateKey(secret), p = publicKey(pub);
    Secret keys(64);
    shared(k.get(), p.get(), keys);
    auto expected = mac(data.left(data.size() - 32), keys);
    check(CRYPTO_memcmp(ptr(tag), ptr(expected), 32) == 0, "ECIES authentication failed");
    Cipher c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    check(bool(c));
    Wiped plain{QByteArray(size + 16, 0)};
    int n = 0, tail = 0;
    check(EVP_DecryptInit_ex(c.get(), EVP_aes_256_cbc(), nullptr, keys.data(), ptr(iv)) == 1 &&
          EVP_DecryptUpdate(c.get(), out(plain.b), &n, ptr(ciphertext), size) == 1 &&
          EVP_DecryptFinal_ex(c.get(), out(plain.b) + n, &tail) == 1);
    plain.b.resize(n + tail);
    return std::move(plain.b);
}
struct Derived {
    Secret key{32};
    QByteArray tag;
    explicit Derived(const ntb_address &a, bool twice) {
        QByteArray data;
        var(data, a.version);
        var(data, a.stream);
        data.append(reinterpret_cast<const char *>(a.ripe), 20);
        Secret digest(64);
        SHA512(ptr(data), data.size(), digest.data());
        if (twice) {
            Secret second(64);
            SHA512(digest.data(), 64, second.data());
            digest = std::move(second);
        }
        memcpy(key.data(), digest.data(), 32);
        tag = QByteArray(reinterpret_cast<const char *>(digest.data() + 32), 32);
    }
    QByteArray pub() {
        auto k = privateKey(key.data());
        return publicBytes(k.get());
    }
};
QByteArray identityFields(const PublicIdentity &i, const ntb_address &a, bool includeVersion) {
    QByteArray b;
    if (includeVersion) {
        var(b, a.version);
        var(b, a.stream);
    }
    number(b, i.behaviors, 4);
    b += i.signingKey.mid(1);
    b += i.encryptionKey.mid(1);
    if (a.version >= 3) {
        var(b, i.nonceTrials);
        var(b, i.extraBytes);
    }
    return b;
}
PublicIdentity readIdentity(Reader &r, quint64 version = 0, quint64 stream = 0) {
    if (version == 0) {
        version = r.var();
        stream = r.var();
    }
    check(version >= 2 && version <= 4 && stream == 1);
    PublicIdentity i;
    i.behaviors = r.number(4);
    i.signingKey = QByteArray(1, 4) + r.take(64);
    i.encryptionKey = QByteArray(1, 4) + r.take(64);
    auto s = publicKey(i.signingKey), e = publicKey(i.encryptionKey);
    if (version >= 3) {
        i.nonceTrials = r.var();
        i.extraBytes = r.var();
    }
    ntb_address a{};
    ntb_address_from_network_keys(&a, version, stream, ptr(i.signingKey) + 1,
                                  ptr(i.encryptionKey) + 1);
    i.address = name(a);
    return i;
}
QByteArray textFields(const QString &subject, const QString &body) {
    check(!subject.contains('\n') && !subject.contains('\r'), "Subject must be a single line");
    Wiped t{("Subject:" + subject + "\nBody:" + body).toUtf8()};
    check(t.b.size() <= MaxText, "Message is too large");
    QByteArray b;
    var(b, 2);
    blob(b, t.b);
    return b;
}
void readText(Reader &r, Message &m) {
    auto encoding = r.var();
    check(encoding == 1 || encoding == 2);
    Wiped bytes{r.blob(MaxText)};
    auto text = QString::fromUtf8(bytes.b);
    auto split = text.indexOf("\nBody:");
    if (encoding == 2 && text.startsWith("Subject:") && split >= 8) {
        m.subject = text.mid(8, split - 8);
        m.body = text.mid(split + 6);
    } else
        m.body = text;
}
QByteArray unframe(const QByteArray &f) {
    Reader r{f};
    check(r.number(4) == 0xe9beb4d9);
    check(r.take(12) == QByteArray("object\0\0\0\0\0\0", 12));
    auto size = r.number(4);
    check(size <= MaxObject);
    auto checksum = r.take(4);
    auto object = r.take(size);
    r.end();
    check(hash(object).left(4) == checksum);
    check(Wire::header(object).has_value());
    return object;
}
void fill(DecodedEnvelope &e, const QByteArray &o, const QString &to, const QString &folder) {
    e.message.hash = Protocol::inventoryHash(o);
    e.message.from = e.sender.address;
    e.message.to = to;
    e.message.folder = folder;
    e.message.received = QDateTime::currentSecsSinceEpoch();
}
} // namespace
std::optional<ObjectHeader> Wire::header(const QByteArray &object) {
    try {
        Reader r{object};
        return parseHeader(r);
    } catch (const std::exception &) {
        return {};
    }
}
bool Wire::validAddress(const QString &s) {
    try {
        address(s);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
PublicIdentity Wire::publicIdentity(const Identity &i) {
    auto s = privateKey(i.keys.data()), e = privateKey(i.keys.data() + 32);
    PublicIdentity p;
    p.address = i.address;
    p.chan = i.chan;
    p.signingKey = publicBytes(s.get());
    p.encryptionKey = publicBytes(e.get());
    boundAddress(p);
    return p;
}
QByteArray Wire::encodeMessage(const Identity &sender, const PublicIdentity &recipient,
                               const QString &subject, const QString &body, qint64 expires,
                               const QByteArray &ackObject) {
    auto s = publicIdentity(sender);
    auto sa = address(s.address), ra = boundAddress(recipient);
    auto h = base(2, 1, expires);
    Wiped plain{identityFields(s, sa, true)};
    plain.b.append(reinterpret_cast<const char *>(ra.ripe), 20);
    plain.b += textFields(subject, body);
    if (!ackObject.isEmpty())
        check(!acknowledgmentToken(ackObject).isEmpty(), "Invalid acknowledgment object");
    blob(plain.b, ackObject.isEmpty() ? QByteArray() : frame(ackObject));
    Wiped signedData{h.mid(8) + plain.b};
    blob(plain.b, signature(sender, signedData.b));
    auto result = h + encrypt(plain.b, recipient.encryptionKey);
    check(result.size() <= MaxObject);
    return result;
}
std::optional<DecodedEnvelope> Wire::decodeMessage(const QByteArray &object,
                                                   const Identity &recipient) {
    try {
        Reader wire{object};
        auto h = parseHeader(wire);
        check(h.type == 2 && h.version == 1);
        auto ra = address(recipient.address);
        Wiped plain{decrypt(object.mid(h.headerSize), recipient.keys.data() + 32)};
        Reader r{plain.b};
        DecodedEnvelope e;
        e.sender = readIdentity(r);
        auto ripe = r.take(20);
        check(CRYPTO_memcmp(ptr(ripe), ra.ripe, 20) == 0);
        readText(r, e.message);
        auto ack = r.blob(MaxObject + 24);
        if (!ack.isEmpty())
            e.acknowledgment = unframe(ack);
        Wiped signedData{object.mid(8, h.headerSize - 8) + plain.b.left(r.pos)};
        verify(e.sender.signingKey, signedData.b, r);
        fill(e, object, recipient.address, recipient.chan ? "Channels" : "Inbox");
        return e;
    } catch (const std::exception &) {
        return {};
    }
}
QByteArray Wire::encodePubkey(const Identity &i, qint64 expires) {
    auto p = publicIdentity(i);
    auto a = address(i.address);
    auto h = base(1, a.version, expires);
    auto plain = identityFields(p, a, false);
    if (a.version == 2)
        return h + plain;
    if (a.version == 3) {
        blob(plain, signature(i, h.mid(8) + plain));
        return h + plain;
    }
    Derived d(a, true);
    h += d.tag;
    blob(plain, signature(i, h.mid(8) + plain));
    return h + encrypt(plain, d.pub());
}
std::optional<PublicIdentity> Wire::decodePubkey(const QByteArray &object,
                                                 const QString &requested) {
    try {
        auto a = address(requested);
        Reader wire{object};
        auto h = parseHeader(wire);
        check(h.type == 1 && h.version == a.version && h.stream == a.stream);
        QByteArray prefix = object.left(h.headerSize), plain;
        if (a.version == 4) {
            Derived d(a, true);
            check(wire.take(32) == d.tag);
            prefix += d.tag;
            plain = decrypt(object.mid(wire.pos), d.key.data());
        } else
            plain = object.mid(wire.pos);
        Reader r{plain};
        auto p = readIdentity(r, a.version, a.stream);
        check(p.address == requested);
        if (a.version >= 3)
            verify(p.signingKey, prefix.mid(8) + plain.left(r.pos), r);
        else
            r.end();
        return p;
    } catch (const std::exception &) {
        return {};
    }
}
QByteArray Wire::getPubkey(const QString &requested, qint64 expires) {
    auto a = address(requested);
    auto b = base(0, a.version, expires);
    if (a.version == 4) {
        Derived d(a, true);
        b += d.tag;
    } else
        b.append(reinterpret_cast<const char *>(a.ripe), 20);
    return b;
}
bool Wire::requestsIdentity(const QByteArray &object, const Identity &i) {
    try {
        Reader r{object};
        auto h = parseHeader(r);
        check(h.type == 0);
        auto expected = getPubkey(i.address, h.expires);
        return expected.mid(8) == object.mid(8);
    } catch (const std::exception &) {
        return false;
    }
}
QByteArray Wire::acknowledgment(const QByteArray &token, qint64 expires) {
    check(token.size() == 32, "Acknowledgment token must have 32 bytes");
    return base(2, 1, expires) + token;
}
QByteArray Wire::acknowledgmentToken(const QByteArray &object) {
    auto h = header(object);
    if (!h || h->type != 2 || h->version != 1 || object.size() != h->headerSize + 32)
        return {};
    return object.mid(h->headerSize);
}
QByteArray Wire::encodeBroadcast(const Identity &i, const QString &subject, const QString &body,
                                 qint64 expires) {
    auto p = publicIdentity(i);
    auto a = address(i.address);
    auto h = base(3, a.version < 4 ? 4 : 5, expires);
    Derived d(a, a.version >= 4);
    if (a.version >= 4)
        h += d.tag;
    Wiped plain{identityFields(p, a, true)};
    plain.b += textFields(subject, body);
    Wiped signedData{h.mid(8) + plain.b};
    blob(plain.b, signature(i, signedData.b));
    auto result = h + encrypt(plain.b, d.pub());
    check(result.size() <= MaxObject);
    return result;
}
std::optional<DecodedEnvelope> Wire::decodeBroadcast(const QByteArray &object,
                                                     const QString &subscribed) {
    try {
        auto a = address(subscribed);
        Reader wire{object};
        auto h = parseHeader(wire);
        check(h.type == 3 && h.version == (a.version < 4 ? 4 : 5));
        Derived d(a, a.version >= 4);
        if (a.version >= 4)
            check(wire.take(32) == d.tag);
        Wiped plain{decrypt(object.mid(wire.pos), d.key.data())};
        Reader r{plain.b};
        DecodedEnvelope e;
        e.broadcast = true;
        e.sender = readIdentity(r);
        check(e.sender.address == subscribed);
        readText(r, e.message);
        Wiped signedData{object.mid(8, wire.pos - 8) + plain.b.left(r.pos)};
        verify(e.sender.signingKey, signedData.b, r);
        fill(e, object, subscribed, "Broadcasts");
        return e;
    } catch (const std::exception &) {
        return {};
    }
}
QByteArray Wire::frame(const QByteArray &object) {
    check(header(object).has_value());
    QByteArray b;
    number(b, 0xe9beb4d9, 4);
    b.append("object\0\0\0\0\0\0", 12);
    number(b, object.size(), 4);
    b += hash(object).left(4);
    return b + object;
}
} // namespace bm
