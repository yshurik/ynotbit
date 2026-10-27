#include <algorithm>
#include <bmc/clock.h>
#include <bmc/hash.h>
#include <bmc/protocol.h>
#include <bmc/text.h>
#include <bmc/wire.h>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <sodium.h>
extern "C" {
#include "config.h"
#include "ntb-address.h"
}

namespace bmc {
namespace {
constexpr int MaxObject = kMaxObjectBytes;

void check(bool ok, const char *what = "Invalid wire object") {
    if (!ok)
        throw Error(Code::Invalid, what);
}
bool same(ByteView a, ByteView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

struct Wiped {
    Bytes b;
    ~Wiped() {
        if (!b.empty())
            sodium_memzero(b.data(), b.size());
    }
};
using Key = std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)>;
using Bn = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
using Point = std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)>;
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

struct Reader {
    ByteView b;
    size_t pos = 0;
    ByteView take(uint64_t n) {
        check(n <= uint64_t(b.size() - pos));
        const auto v = b.subspan(pos, size_t(n));
        pos += size_t(n);
        return v;
    }
    uint64_t number(int n) {
        uint64_t r = 0;
        for (auto c : take(uint64_t(n)))
            r = (r << 8) | c;
        return r;
    }
    uint64_t var() {
        auto n = number(1);
        if (n < 253)
            return n;
        const int size = n == 253 ? 2 : n == 254 ? 4 : 8;
        n = number(size);
        check(n >= (size == 2 ? 253ULL : size == 4 ? 65536ULL : 4294967296ULL));
        return n;
    }
    ByteView blob(uint64_t max) {
        const auto n = var();
        check(n <= max);
        return take(n);
    }
    void end() {
        check(pos == b.size());
    }
};
void putBlob(Bytes &b, ByteView v) {
    putVarInt(b, v.size());
    append(b, v);
}
std::string name(const ntb_address &a) {
    char buf[NTB_ADDRESS_MAX_LENGTH + 1];
    ntb_address_encode(&a, buf);
    return buf;
}
ntb_address address(std::string_view s) {
    ntb_address a{};
    const std::string text(s);
    check(s.size() <= 64 && s.substr(0, 3) == "BM-" && text.find('\0') == std::string::npos &&
              ntb_address_decode(&a, text.c_str()) && a.version >= 2 && a.version <= 4 &&
              a.stream == 1 && name(a) == s,
          "Invalid Bitmessage address");
    return a;
}
// withPublic: also compute the public point (a full scalar multiplication).
// Signing and addresses need it; ECDH does not, and decrypting tries every
// identity on every incoming message, so it skips it.
Key privateKey(const unsigned char *secret, bool withPublic = true) {
    Key k(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    check(bool(k));
    Bn n(BN_bin2bn(secret, 32, nullptr), BN_clear_free);
    Bn order(BN_new(), BN_clear_free);
    auto group = EC_KEY_get0_group(k.get());
    check(n && order && EC_GROUP_get_order(group, order.get(), nullptr) == 1 &&
              !BN_is_zero(n.get()) && BN_cmp(n.get(), order.get()) < 0,
          "Invalid private scalar");
    check(EC_KEY_set_private_key(k.get(), n.get()) == 1);
    if (!withPublic)
        return k;
    Point p(EC_POINT_new(group), EC_POINT_free);
    check(p && EC_POINT_mul(group, p.get(), n.get(), nullptr, nullptr, nullptr) == 1 &&
          EC_KEY_set_public_key(k.get(), p.get()) == 1);
    return k;
}
Key publicKey(ByteView bytes) {
    check(bytes.size() == 65 && bytes[0] == 4, "Invalid public key");
    Key k(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    check(bool(k));
    Point p(EC_POINT_new(EC_KEY_get0_group(k.get())), EC_POINT_free);
    check(p &&
              EC_POINT_oct2point(EC_KEY_get0_group(k.get()), p.get(), bytes.data(), 65, nullptr) ==
                  1 &&
              EC_POINT_is_at_infinity(EC_KEY_get0_group(k.get()), p.get()) == 0 &&
              EC_KEY_set_public_key(k.get(), p.get()) == 1 && EC_KEY_check_key(k.get()) == 1,
          "Invalid public point");
    return k;
}
Bytes publicBytes(EC_KEY *k) {
    Bytes b(65, 0);
    check(EC_POINT_point2oct(EC_KEY_get0_group(k), EC_KEY_get0_public_key(k),
                             POINT_CONVERSION_UNCOMPRESSED, b.data(), 65, nullptr) == 65);
    return b;
}
ntb_address boundAddress(const PublicIdentity &i) {
    auto a = address(i.address);
    auto s = publicKey(i.signingKey), e = publicKey(i.encryptionKey);
    (void)s;
    (void)e;
    ntb_address derived{};
    ntb_address_from_network_keys(&derived, a.version, a.stream, i.signingKey.data() + 1,
                                  i.encryptionKey.data() + 1);
    check(ntb_address_equal(&derived, &a), "Public keys do not match address");
    return a;
}
Bytes base(uint32_t type, uint64_t version, int64_t expires) {
    check(expires > 0, "Invalid expiry");
    Bytes b(8, 0);
    putBE(b, uint64_t(expires), 8);
    putBE(b, type, 4);
    putVarInt(b, version);
    putVarInt(b, 1);
    return b;
}
ObjectHeader parseHeader(Reader &r) {
    check(r.b.size() <= size_t(MaxObject));
    r.number(8);
    auto expiry = r.number(8);
    check(expiry > 0 && expiry <= uint64_t(std::numeric_limits<int64_t>::max()));
    auto type = uint32_t(r.number(4));
    auto version = r.var(), stream = r.var();
    check(stream == 1);
    return {type, version, stream, int64_t(expiry), int(r.pos)};
}
Bytes signature(IdentityRef i, ByteView data) {
    auto k = privateKey(i.keys);
    const auto digest = hash(Digest::Sha256, data);
    Bytes sig(size_t(ECDSA_size(k.get())), 0);
    unsigned int n = unsigned(sig.size());
    check(ECDSA_sign(0, digest.data(), int(digest.size()), sig.data(), &n, k.get()) == 1,
          "Signing failed");
    sig.resize(n);
    return sig;
}
void verify(ByteView key, ByteView data, Reader &r) {
    auto sig = r.blob(80);
    r.end();
    auto k = publicKey(key);
    for (auto algorithm : {Digest::Sha256, Digest::Sha1}) {
        const auto d = hash(algorithm, data);
        if (ECDSA_verify(0, d.data(), int(d.size()), sig.data(), int(sig.size()), k.get()) == 1)
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
// The same secret through libsecp256k1, for decrypting: Bitmessage hashes the
// shared point's bare x coordinate (what ECDH_compute_key returns), so the
// hash step only copies it out. 10-20x faster than OpenSSL's generic curve
// code, and decrypting runs once per identity on every incoming message.
int sharedX(unsigned char *out, const unsigned char *x32, const unsigned char *, void *) {
    std::memcpy(out, x32, 32);
    return 1;
}
void sharedFast(const unsigned char *secret, ByteView uncompressed, Secret &keys) {
    secp256k1_pubkey point;
    check(uncompressed.size() == 65 && secp256k1_ec_pubkey_parse(secp256k1_context_static, &point,
                                                                 uncompressed.data(), 65) == 1,
          "Invalid public key");
    Secret raw(32);
    check(secp256k1_ecdh(secp256k1_context_static, raw.data(), &point, secret, sharedX, nullptr) ==
              1,
          "ECDH failed");
    check(SHA512(raw.data(), 32, keys.data()) != nullptr);
}
Bytes mac(ByteView data, const Secret &keys) {
    Bytes result(32, 0);
    unsigned int n = 32;
    check(HMAC(EVP_sha256(), keys.data() + 32, 32, data.data(), data.size(), result.data(), &n) &&
          n == 32);
    return result;
}
Bytes encrypt(ByteView plain, ByteView recipient) {
    auto pub = publicKey(recipient);
    Key ephemeral(EC_KEY_new_by_curve_name(NID_secp256k1), EC_KEY_free);
    check(ephemeral && EC_KEY_generate_key(ephemeral.get()) == 1);
    Secret keys(64);
    shared(ephemeral.get(), pub.get(), keys);
    Bytes result(16, 0);
    check(RAND_bytes(result.data(), 16) == 1);
    const auto p = publicBytes(ephemeral.get());
    putBE(result, NID_secp256k1, 2);
    putBE(result, 32, 2);
    append(result, ByteView(p).subspan(1, 32));
    putBE(result, 32, 2);
    append(result, ByteView(p).subspan(33, 32));
    Cipher c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    check(bool(c));
    check(EVP_EncryptInit_ex(c.get(), EVP_aes_256_cbc(), nullptr, keys.data(), result.data()) == 1);
    Bytes encrypted(plain.size() + 16, 0);
    int n = 0, tail = 0;
    check(EVP_EncryptUpdate(c.get(), encrypted.data(), &n, plain.data(), int(plain.size())) == 1 &&
          EVP_EncryptFinal_ex(c.get(), encrypted.data() + n, &tail) == 1);
    encrypted.resize(size_t(n + tail));
    append(result, encrypted);
    append(result, mac(result, keys));
    return result;
}
Bytes decrypt(ByteView data, const unsigned char *secret) {
    check(data.size() <= size_t(MaxObject));
    Reader r{data};
    const auto iv = r.take(16);
    check(r.number(2) == NID_secp256k1);
    Bytes pub(1, 4);
    for (int j = 0; j < 2; ++j) {
        const auto n = r.number(2);
        check(n > 0 && n <= 32);
        pub.insert(pub.end(), size_t(32 - n), uint8_t(0));
        append(pub, r.take(n));
    }
    const int size = int(data.size()) - int(r.pos) - 32;
    check(size >= 16 && size % 16 == 0);
    const auto ciphertext = r.take(uint64_t(size)), tag = r.take(32);
    r.end();
    Secret keys(64);
    sharedFast(secret, pub, keys);
    const auto expected = mac(data.first(data.size() - 32), keys);
    check(CRYPTO_memcmp(tag.data(), expected.data(), 32) == 0, "ECIES authentication failed");
    Cipher c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    check(bool(c));
    Wiped plain{Bytes(size_t(size) + 16, 0)};
    int n = 0, tail = 0;
    check(EVP_DecryptInit_ex(c.get(), EVP_aes_256_cbc(), nullptr, keys.data(), iv.data()) == 1 &&
          EVP_DecryptUpdate(c.get(), plain.b.data(), &n, ciphertext.data(), size) == 1 &&
          EVP_DecryptFinal_ex(c.get(), plain.b.data() + n, &tail) == 1);
    plain.b.resize(size_t(n + tail));
    return std::move(plain.b);
}
struct Derived {
    Secret key{32};
    Bytes tag;
    explicit Derived(const ntb_address &a, bool twice) {
        Bytes data;
        putVarInt(data, a.version);
        putVarInt(data, a.stream);
        append(data, ByteView(a.ripe, 20));
        Secret digest(64);
        SHA512(data.data(), data.size(), digest.data());
        if (twice) {
            Secret second(64);
            SHA512(digest.data(), 64, second.data());
            digest = std::move(second);
        }
        memcpy(key.data(), digest.data(), 32);
        tag.assign(digest.data() + 32, digest.data() + 64);
    }
    Bytes pub() {
        auto k = privateKey(key.data());
        return publicBytes(k.get());
    }
};
Bytes identityFields(const PublicIdentity &i, const ntb_address &a, bool includeVersion) {
    Bytes b;
    if (includeVersion) {
        putVarInt(b, a.version);
        putVarInt(b, a.stream);
    }
    putBE(b, i.behaviors, 4);
    append(b, ByteView(i.signingKey).subspan(1));
    append(b, ByteView(i.encryptionKey).subspan(1));
    if (a.version >= 3) {
        putVarInt(b, i.nonceTrials);
        putVarInt(b, i.extraBytes);
    }
    return b;
}
PublicIdentity readIdentity(Reader &r, uint64_t version = 0, uint64_t stream = 0) {
    if (version == 0) {
        version = r.var();
        stream = r.var();
    }
    check(version >= 2 && version <= 4 && stream == 1);
    PublicIdentity i;
    i.behaviors = uint32_t(r.number(4));
    i.signingKey = Bytes(1, 4);
    append(i.signingKey, r.take(64));
    i.encryptionKey = Bytes(1, 4);
    append(i.encryptionKey, r.take(64));
    auto s = publicKey(i.signingKey), e = publicKey(i.encryptionKey);
    (void)s;
    (void)e;
    if (version >= 3) {
        i.nonceTrials = r.var();
        i.extraBytes = r.var();
    }
    ntb_address a{};
    ntb_address_from_network_keys(&a, int(version), int(stream), i.signingKey.data() + 1,
                                  i.encryptionKey.data() + 1);
    i.address = name(a);
    return i;
}
// "Subject:<subject>\nBody:<body>" as the bytes that go on the wire.
Wiped letterText(std::string_view subject, std::string_view body) {
    check(subject.find('\n') == std::string_view::npos &&
              subject.find('\r') == std::string_view::npos,
          "Subject must be a single line");
    check(text::validUtf8(subject) && text::validUtf8(body), "Text must be valid UTF-8");
    return Wiped{bytesOf("Subject:" + std::string(subject) + "\nBody:" + std::string(body))};
}
// The encoding (2) and the text, as a message carries them.
Bytes textFields(ByteView letter) {
    check(letter.size() <= size_t(kMaxLetterText), "Message is too large");
    Bytes b;
    putVarInt(b, 2);
    putBlob(b, letter);
    return b;
}
void readText(Reader &r, Message &m) {
    const auto encoding = r.var();
    check(encoding == 1 || encoding == 2);
    // Other clients may fill a whole object; the object's own size bounds it.
    Wiped bytes{bytesOf(r.blob(kMaxObjectBytes))};
    const auto letter = text::sanitizeUtf8(chars(bytes.b));
    const auto split = letter.find("\nBody:");
    if (encoding == 2 && letter.starts_with("Subject:") && split != std::string::npos &&
        split >= 8) {
        m.subject = letter.substr(8, split - 8);
        m.body = letter.substr(split + 6);
    } else {
        m.body = letter;
    }
}
Bytes unframe(ByteView f) {
    Reader r{f};
    check(r.number(4) == 0xe9beb4d9);
    static const uint8_t command[12] = {'o', 'b', 'j', 'e', 'c', 't', 0, 0, 0, 0, 0, 0};
    check(same(r.take(12), ByteView(command, 12)));
    const auto size = r.number(4);
    check(size <= uint64_t(MaxObject));
    const auto checksum = r.take(4);
    const auto object = r.take(size);
    r.end();
    check(same(checksum, ByteView(hash(Digest::Sha512, object)).first(4)));
    check(Wire::header(object).has_value());
    return bytesOf(object);
}
void fill(DecodedEnvelope &e, ByteView o, std::string_view to, const char *folder) {
    e.message.hash = Protocol::inventoryHash(o);
    e.message.from = e.sender.address;
    e.message.to = std::string(to);
    e.message.folder = folder;
    e.message.received = now();
}
// A msg whose text is made by makeText at the point the old code made it, so that the first error
// is the same one.
Bytes encodeWith(IdentityRef sender, const PublicIdentity &recipient,
                 const std::function<Bytes()> &makeText, int64_t expires, ByteView ackObject) {
    auto s = Wire::publicIdentity(sender);
    auto sa = address(s.address), ra = boundAddress(recipient);
    auto h = base(2, 1, expires);
    Wiped plain{identityFields(s, sa, true)};
    append(plain.b, ByteView(ra.ripe, 20));
    append(plain.b, makeText());
    if (!ackObject.empty())
        check(!Wire::acknowledgmentToken(ackObject).empty(), "Invalid acknowledgment object");
    putBlob(plain.b, ackObject.empty() ? Bytes() : Wire::frame(ackObject));
    Wiped signedData{concat({ByteView(h).subspan(8), ByteView(plain.b)})};
    putBlob(plain.b, signature(sender, signedData.b));
    auto result = h;
    append(result, encrypt(plain.b, recipient.encryptionKey));
    check(result.size() <= size_t(MaxObject));
    return result;
}
} // namespace

std::optional<ObjectHeader> Wire::header(ByteView object) {
    try {
        Reader r{object};
        return parseHeader(r);
    } catch (const std::exception &) {
        return {};
    }
}

bool Wire::validAddress(std::string_view s) {
    try {
        address(s);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

PublicIdentity Wire::publicIdentity(IdentityRef i) {
    auto s = privateKey(i.keys), e = privateKey(i.keys + 32);
    PublicIdentity p;
    p.address = std::string(i.address);
    p.chan = i.chan;
    p.signingKey = publicBytes(s.get());
    p.encryptionKey = publicBytes(e.get());
    boundAddress(p);
    return p;
}

Bytes Wire::encodeMessage(IdentityRef sender, const PublicIdentity &recipient,
                          std::string_view subject, std::string_view body, int64_t expires,
                          ByteView ackObject) {
    return encodeWith(
        sender, recipient, [&] { return textFields(letterText(subject, body).b); }, expires,
        ackObject);
}

Bytes Wire::encodeMessageText(IdentityRef sender, const PublicIdentity &recipient, ByteView text,
                              int64_t expires, ByteView ackObject) {
    return encodeWith(sender, recipient, [&] { return textFields(text); }, expires, ackObject);
}

std::optional<DecodedEnvelope> Wire::decodeMessage(ByteView object, IdentityRef recipient) {
    try {
        Reader wire{object};
        auto h = parseHeader(wire);
        check(h.type == 2 && h.version == 1);
        // Decrypting fails for nearly every identity tried; only then is the
        // recipient's address worth decoding.
        Wiped plain{decrypt(object.subspan(size_t(h.headerSize)), recipient.keys + 32)};
        auto ra = address(recipient.address);
        Reader r{plain.b};
        DecodedEnvelope e;
        e.sender = readIdentity(r);
        const auto ripe = r.take(20);
        check(CRYPTO_memcmp(ripe.data(), ra.ripe, 20) == 0);
        readText(r, e.message);
        const auto ack = r.blob(uint64_t(MaxObject) + 24);
        if (!ack.empty())
            e.acknowledgment = unframe(ack);
        Wiped signedData{
            concat({object.subspan(8, size_t(h.headerSize) - 8), ByteView(plain.b).first(r.pos)})};
        verify(e.sender.signingKey, signedData.b, r);
        fill(e, object, recipient.address, recipient.chan ? "Channels" : "Inbox");
        return e;
    } catch (const std::exception &) {
        return {};
    }
}

Bytes Wire::encodePubkey(IdentityRef i, int64_t expires) {
    auto p = publicIdentity(i);
    auto a = address(i.address);
    auto h = base(1, a.version, expires);
    auto plain = identityFields(p, a, false);
    if (a.version == 2) {
        append(h, plain);
        return h;
    }
    if (a.version == 3) {
        putBlob(plain, signature(i, concat({ByteView(h).subspan(8), ByteView(plain)})));
        append(h, plain);
        return h;
    }
    Derived d(a, true);
    append(h, d.tag);
    putBlob(plain, signature(i, concat({ByteView(h).subspan(8), ByteView(plain)})));
    append(h, encrypt(plain, d.pub()));
    return h;
}

std::optional<PublicIdentity> Wire::decodePubkey(ByteView object, std::string_view requested) {
    try {
        auto a = address(requested);
        Reader wire{object};
        auto h = parseHeader(wire);
        check(h.type == 1 && h.version == a.version && h.stream == a.stream);
        Bytes prefix(object.begin(), object.begin() + h.headerSize), plain;
        if (a.version == 4) {
            Derived d(a, true);
            check(same(wire.take(32), d.tag));
            append(prefix, d.tag);
            plain = decrypt(object.subspan(wire.pos), d.key.data());
        } else {
            plain = bytesOf(object.subspan(wire.pos));
        }
        Reader r{plain};
        auto p = readIdentity(r, a.version, a.stream);
        check(p.address == requested);
        if (a.version >= 3)
            verify(p.signingKey,
                   concat({ByteView(prefix).subspan(8), ByteView(plain).first(r.pos)}), r);
        else
            r.end();
        return p;
    } catch (const std::exception &) {
        return {};
    }
}

Bytes Wire::getPubkey(std::string_view requested, int64_t expires) {
    auto a = address(requested);
    auto b = base(0, a.version, expires);
    if (a.version == 4) {
        Derived d(a, true);
        append(b, d.tag);
    } else {
        append(b, ByteView(a.ripe, 20));
    }
    return b;
}

bool Wire::requestsIdentity(ByteView object, IdentityRef i) {
    try {
        Reader r{object};
        auto h = parseHeader(r);
        check(h.type == 0);
        const auto expected = getPubkey(i.address, h.expires);
        return same(ByteView(expected).subspan(8), object.subspan(8));
    } catch (const std::exception &) {
        return false;
    }
}

Bytes Wire::acknowledgment(ByteView token, int64_t expires) {
    check(token.size() == 32, "Acknowledgment token must have 32 bytes");
    auto b = base(2, 1, expires);
    append(b, token);
    return b;
}

Bytes Wire::acknowledgmentToken(ByteView object) {
    auto h = header(object);
    if (!h || h->type != 2 || h->version != 1 || object.size() != size_t(h->headerSize) + 32)
        return {};
    return bytesOf(object.subspan(size_t(h->headerSize)));
}

Bytes Wire::encodeBroadcast(IdentityRef i, std::string_view subject, std::string_view body,
                            int64_t expires) {
    auto p = publicIdentity(i);
    auto a = address(i.address);
    auto h = base(3, a.version < 4 ? 4 : 5, expires);
    Derived d(a, a.version >= 4);
    if (a.version >= 4)
        append(h, d.tag);
    Wiped plain{identityFields(p, a, true)};
    append(plain.b, textFields(letterText(subject, body).b));
    Wiped signedData{concat({ByteView(h).subspan(8), ByteView(plain.b)})};
    putBlob(plain.b, signature(i, signedData.b));
    append(h, encrypt(plain.b, d.pub()));
    check(h.size() <= size_t(MaxObject));
    return h;
}

std::optional<DecodedEnvelope> Wire::decodeBroadcast(ByteView object, std::string_view subscribed) {
    try {
        auto a = address(subscribed);
        Reader wire{object};
        auto h = parseHeader(wire);
        check(h.type == 3 && h.version == uint64_t(a.version < 4 ? 4 : 5));
        Derived d(a, a.version >= 4);
        if (a.version >= 4)
            check(same(wire.take(32), d.tag));
        Wiped plain{decrypt(object.subspan(wire.pos), d.key.data())};
        Reader r{plain.b};
        DecodedEnvelope e;
        e.broadcast = true;
        e.sender = readIdentity(r);
        check(e.sender.address == subscribed);
        readText(r, e.message);
        Wiped signedData{concat({object.subspan(8, wire.pos - 8), ByteView(plain.b).first(r.pos)})};
        verify(e.sender.signingKey, signedData.b, r);
        fill(e, object, subscribed, "Broadcasts");
        return e;
    } catch (const std::exception &) {
        return {};
    }
}

std::optional<Bytes> Wire::tagOfAddress(std::string_view subscribed) {
    try {
        const auto a = address(subscribed);
        if (a.version < 4)
            return Bytes{};
        return Derived(a, true).tag;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

Bytes Wire::tagOfBroadcast(ByteView object) {
    const auto h = header(object);
    if (!h || h->type != 3 || h->version != 5 || object.size() < size_t(h->headerSize) + 32)
        return {};
    return bytesOf(object.subspan(size_t(h->headerSize), 32));
}

Bytes Wire::frame(ByteView object) {
    check(header(object).has_value());
    Bytes b;
    putBE(b, 0xe9beb4d9, 4);
    append(b, view(std::string_view("object\0\0\0\0\0\0", 12)));
    putBE(b, object.size(), 4);
    append(b, ByteView(hash(Digest::Sha512, object)).first(4));
    append(b, object);
    return b;
}
} // namespace bmc
