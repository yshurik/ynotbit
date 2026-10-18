#include <bmc/clock.h>
#include <bmc/hash.h>
#include <bmc/protocol.h>
#include <bmc/text.h>
#include <cstring>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/sha.h>
#include <sodium.h>
#include <stdexcept>
extern "C" {
#include "config.h"
#include "ntb-address.h"
#include "ntb-ecc.h"
#include "ntb-proto.h"
}

namespace bmc {
namespace {
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
Bytes toBytes(const Buffer &b) {
    return Bytes(b.b.data, b.b.data + b.b.length);
}
ntb_address address(IdentityRef i) {
    ntb_address a;
    const std::string text(i.address);
    require(ntb_address_decode(&a, text.c_str()), "Invalid identity address");
    return a;
}
Bytes pub(Ecc &e, const unsigned char *secret) {
    Bytes b(65, 0);
    ntb_ecc_make_pub_key_bin(e.p, secret, b.data());
    return b;
}
std::string name(const ntb_address &a) {
    char output[NTB_ADDRESS_MAX_LENGTH + 1];
    ntb_address_encode(&a, output);
    return output;
}
} // namespace

Identity Protocol::fromPrivate(std::string_view label, ByteView sign, ByteView enc, int version,
                               int stream, bool chan) {
    require(sign.size() == 32 && enc.size() == 32, "Private keys must have 32 bytes");
    require(version >= 2 && version <= 4 && stream == 1,
            "Only address versions 2–4 in stream 1 are supported");
    Ecc ecc;
    Key s{ntb_ecc_create_key(ecc.p, sign.data())}, e{ntb_ecc_create_key(ecc.p, enc.data())};
    require(EC_KEY_check_key(s.p) == 1 && EC_KEY_check_key(e.p) == 1, "Invalid private key");
    auto sp = pub(ecc, sign.data()), ep = pub(ecc, enc.data());
    ntb_address a;
    ntb_address_from_network_keys(&a, version, stream, sp.data() + 1, ep.data() + 1);
    Identity i;
    i.label = std::string(label);
    i.address = name(a);
    i.chan = chan;
    memcpy(i.keys.data(), sign.data(), 32);
    memcpy(i.keys.data() + 32, enc.data(), 32);
    return i;
}

Identity Protocol::identity(std::string_view label) {
    Ecc ecc;
    Identity i;
    i.label = std::string(label);
    for (;;) {
        Key s{ntb_ecc_create_random_key(ecc.p)}, e{ntb_ecc_create_random_key(ecc.p)};
        require(s.p && e.p, "Key generation failed");
        BN_bn2binpad(EC_KEY_get0_private_key(s.p), i.keys.data(), 32);
        BN_bn2binpad(EC_KEY_get0_private_key(e.p), i.keys.data() + 32, 32);
        auto sp = pub(ecc, i.keys.data()), ep = pub(ecc, i.keys.data() + 32);
        ntb_address a;
        ntb_address_from_network_keys(&a, 4, 1, sp.data() + 1, ep.data() + 1);
        if (a.ripe[0] == 0) {
            i.address = name(a);
            return i;
        }
    }
}

Identity Protocol::channel(std::string_view phrase, std::string_view label, int version) {
    require(version == 3 || version == 4, "Unsupported channel version");
    require(!phrase.empty(), "Channel phrase cannot be empty");
    std::string pass(phrase);
    Ecc ecc;
    Identity i;
    i.label = std::string(label);
    i.chan = true;
    for (uint64_t nonce = 0; nonce < 1000000; nonce += 2) {
        Buffer sign, enc;
        ntb_buffer_append(&sign.b, pass.data(), pass.size());
        ntb_buffer_append(&enc.b, pass.data(), pass.size());
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
        ntb_address_from_network_keys(&a, version, 1, sp.data() + 1, ep.data() + 1);
        if (a.ripe[0] == 0) {
            i.address = name(a);
            sodium_memzero(pass.data(), pass.size());
            return i;
        }
    }
    sodium_memzero(pass.data(), pass.size());
    throw std::runtime_error("Channel derivation limit reached");
}

Bytes Protocol::encodeMessage(IdentityRef sender, IdentityRef recipient, std::string_view subject,
                              std::string_view body, int64_t expires) {
    require(subject.find('\n') == std::string_view::npos &&
                subject.find('\r') == std::string_view::npos,
            "Subject must be a single line");
    if (!text::validUtf8(subject) || !text::validUtf8(body))
        throw Error(Code::Argument,
                    "Text must be valid UTF-8"); // not a require(): no new catalogue entry
    std::string letter = "Subject:" + std::string(subject) + "\nBody:" + std::string(body);
    require(letter.size() <= size_t(kMaxLetterText), "Message is too large");
    auto sa = address(sender), ra = address(recipient);
    Ecc ecc;
    auto sp = pub(ecc, sender.keys), ep = pub(ecc, sender.keys + 32);
    Buffer header, plain, result;
    ntb_proto_add_64(&header.b, 0);
    ntb_proto_add_64(&header.b, expires);
    ntb_proto_add_32(&header.b, 2);
    ntb_proto_add_var_int(&header.b, 1);
    ntb_proto_add_var_int(&header.b, ra.stream);
    ntb_proto_add_var_int(&plain.b, sa.version);
    ntb_proto_add_var_int(&plain.b, sa.stream);
    ntb_proto_add_32(&plain.b, 0);
    ntb_buffer_append(&plain.b, sp.data() + 1, 64);
    ntb_buffer_append(&plain.b, ep.data() + 1, 64);
    if (sa.version >= 3) {
        ntb_proto_add_var_int(&plain.b, 1000);
        ntb_proto_add_var_int(&plain.b, 1000);
    }
    ntb_buffer_append(&plain.b, ra.ripe, 20);
    ntb_proto_add_var_int(&plain.b, 2);
    ntb_proto_add_var_int(&plain.b, letter.size());
    ntb_buffer_append(&plain.b, letter.data(), letter.size());
    ntb_proto_add_var_int(&plain.b, 0);
    const Bytes headerBytes = toBytes(header), plainBytes = toBytes(plain);
    Bytes signedData = concat({ByteView(headerBytes).subspan(8), ByteView(plainBytes)});
    const Bytes digest = hash(Digest::Sha256, signedData);
    Key key{ntb_ecc_create_key(ecc.p, sender.keys)};
    unsigned char sig[80];
    unsigned int size = 80;
    require(ECDSA_sign(0, digest.data(), digest.size(), sig, &size, key.p) == 1,
            "Message signing failed");
    ntb_proto_add_var_int(&plain.b, size);
    ntb_buffer_append(&plain.b, sig, size);
    ntb_buffer_append(&result.b, header.b.data, header.b.length);
    auto point = ntb_ecc_make_pub_key_point(ecc.p, recipient.keys + 32);
    ntb_ecc_encrypt_with_point(ecc.p, point, plain.b.data, plain.b.length, &result.b);
    EC_POINT_free(point);
    sodium_memzero(letter.data(), letter.size());
    wipe(signedData);
    return toBytes(result);
}

std::optional<Message> Protocol::decodeMessage(ByteView object, IdentityRef recipient) {
    if (object.size() > 262144)
        return {};
    ntb_proto_object_header h;
    auto hs = ntb_proto_get_object_header(object.data(), object.size(), &h);
    if (hs < 0 || h.type != 2 || h.version != 1 || h.stream != 1)
        return {};
    // Bound both EC coordinates before entering legacy ECIES parsing.
    const ByteView encrypted = object.subspan(size_t(hs));
    if (encrypted.size() < 16 + 2 + 2 + 32 + 2 + 32 + 16 + 32)
        return {};
    size_t offset = 18;
    for (int j = 0; j < 2; ++j) {
        if (offset + 2 > encrypted.size())
            return {};
        auto length = ntb_proto_get_16(encrypted.data() + offset);
        offset += 2;
        if (length == 0 || length > 32 || offset + length > encrypted.size())
            return {};
        offset += length;
    }
    Ecc ecc;
    Key key{ntb_ecc_create_key(ecc.p, recipient.keys + 32)};
    Buffer plain;
    if (!ntb_ecc_decrypt(ecc.p, key.p, encrypted.data(), encrypted.size(), &plain.b))
        return {};
    ntb_proto_decrypted_msg m;
    if (!ntb_proto_get_decrypted_msg(plain.b.data, plain.b.length, &m))
        return {};
    auto ra = address(recipient);
    if (CRYPTO_memcmp(ra.ripe, m.destination_ripe, 20) != 0)
        return {};
    if (m.sender_address_version < 2 || m.sender_address_version > 4 ||
        m.sender_stream_number != 1 || m.message_length > size_t(kMaxObjectBytes) ||
        m.sig_length > 80)
        return {};
    uint8_t publicKey[65];
    publicKey[0] = 4;
    memcpy(publicKey + 1, m.sender_signing_key, 64);
    Key signing{EC_KEY_new_by_curve_name(NID_secp256k1)};
    const unsigned char *p = publicKey;
    if (!o2i_ECPublicKey(&signing.p, &p, 65) || EC_KEY_check_key(signing.p) != 1)
        return {};
    const auto verify = [&](Digest algorithm) {
        Hasher sha(algorithm);
        sha.add(object.subspan(8, size_t(hs) - 8));
        sha.add(ByteView(plain.b.data, m.signed_data_length));
        const Bytes digest = sha.result();
        return ECDSA_verify(0, digest.data(), digest.size(), m.sig, m.sig_length, signing.p) == 1;
    };
    if (!verify(Digest::Sha256) && !verify(Digest::Sha1))
        return {};
    ntb_address sender;
    ntb_address_from_network_keys(&sender, m.sender_address_version, m.sender_stream_number,
                                  m.sender_signing_key, m.sender_encryption_key);
    Message result;
    result.hash = inventoryHash(object);
    result.from = name(sender);
    result.to = std::string(recipient.address);
    result.folder = recipient.chan ? "Channels" : "Inbox";
    result.received = now();
    if (m.encoding != 1 && m.encoding != 2)
        return {};
    const auto letter = text::sanitizeUtf8(
        std::string_view(reinterpret_cast<const char *>(m.message), m.message_length));
    if (m.encoding == 2 && letter.starts_with("Subject:")) {
        const auto split = letter.find("\nBody:");
        if (split != std::string::npos) {
            result.subject = letter.substr(8, split - 8);
            result.body = letter.substr(split + 6);
        } else {
            result.body = letter;
        }
    } else {
        result.body = letter;
    }
    return result;
}

std::string Protocol::inventoryHash(ByteView b) {
    const Bytes twice = hash(Digest::Sha512, hash(Digest::Sha512, b));
    return hex(ByteView(twice).first(32));
}
} // namespace bmc
