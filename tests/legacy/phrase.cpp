#include "key_search.h"
#include "protocol.h"
#include <QThread>
#include <QTimer>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <sodium.h>
extern "C" {
#include "config.h"
#include "ntb-address.h"
}
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
namespace legacy {
namespace {
// A Bitmessage variable-length integer.
QByteArray varint(quint64 n) {
    QByteArray v;
    const auto bigEndian = [&](int bytes) {
        for (int shift = 8 * (bytes - 1); shift >= 0; shift -= 8)
            v.append(char(n >> shift));
    };
    if (n < 0xfd) {
        v.append(char(n));
    } else if (n <= 0xffff) {
        v.append(char(0xfd));
        bigEndian(2);
    } else if (n <= 0xffffffffULL) {
        v.append(char(0xfe));
        bigEndian(4);
    } else {
        v.append(char(0xff));
        bigEndian(8);
    }
    return v;
}
// One of a phrase's private keys: SHA-512(phrase + varint(nonce))[:32].
void phraseKey(const QByteArray &phrase, quint64 nonce, unsigned char out[32]) {
    auto input = phrase + varint(nonce);
    unsigned char digest[64];
    SHA512(reinterpret_cast<const unsigned char *>(input.constData()), size_t(input.size()),
           digest);
    std::memcpy(out, digest, 32);
    OPENSSL_cleanse(digest, sizeof digest);
    OPENSSL_cleanse(input.data(), size_t(input.size()));
}
// A libsecp256k1 context of one's own: making public keys needs one.
struct Context {
    secp256k1_context *p = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    ~Context() {
        secp256k1_context_destroy(p);
    }
};
// The zero bytes a key pair's RIPE hash starts with; -1 for a key out of range
// (PyBitmessage would fail on it too).
int pairZeroBytes(const secp256k1_context *ctx, const unsigned char sign[32],
                  const unsigned char enc[32]) {
    unsigned char keys[130];
    secp256k1_pubkey point;
    size_t length = 65;
    if (!secp256k1_ec_pubkey_create(ctx, &point, sign))
        return -1;
    secp256k1_ec_pubkey_serialize(ctx, keys, &length, &point, SECP256K1_EC_UNCOMPRESSED);
    length = 65;
    if (!secp256k1_ec_pubkey_create(ctx, &point, enc))
        return -1;
    secp256k1_ec_pubkey_serialize(ctx, keys + 65, &length, &point, SECP256K1_EC_UNCOMPRESSED);
    ntb_address a;
    ntb_address_from_network_keys(&a, 4, 1, keys + 1, keys + 66);
    int zeros = 0;
    while (zeros < 20 && a.ripe[zeros] == 0)
        ++zeros;
    return zeros;
}
// An identity from its two private keys.
Identity fromKeys(const unsigned char sign[32], const unsigned char enc[32]) {
    QByteArray s(reinterpret_cast<const char *>(sign), 32),
        e(reinterpret_cast<const char *>(enc), 32);
    auto identity = Protocol::fromPrivate({}, s, e, 4, 1, false);
    OPENSSL_cleanse(s.data(), 32);
    OPENSSL_cleanse(e.data(), 32);
    return identity;
}
} // namespace
Identity phraseIdentity(const QString &phrase, const QString &label, quint64 pair) {
    auto bytes = phrase.toUtf8();
    unsigned char sign[32], enc[32];
    phraseKey(bytes, 2 * pair, sign);
    phraseKey(bytes, 2 * pair + 1, enc);
    auto identity = fromKeys(sign, enc);
    identity.label = label;
    OPENSSL_cleanse(sign, 32);
    OPENSSL_cleanse(enc, 32);
    OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    return identity;
}
std::optional<quint64> firstPair(const QString &phrase, int zeroBytes, quint64 limit) {
    auto bytes = phrase.toUtf8();
    Context ctx;
    unsigned char sign[32], enc[32];
    std::optional<quint64> found;
    for (quint64 pair = 0; pair < limit && !found; ++pair) {
        phraseKey(bytes, 2 * pair, sign);
        phraseKey(bytes, 2 * pair + 1, enc);
        if (pairZeroBytes(ctx.p, sign, enc) >= zeroBytes)
            found = pair;
    }
    OPENSSL_cleanse(sign, 32);
    OPENSSL_cleanse(enc, 32);
    OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    return found;
}
int zeroBytes(const QString &address) {
    ntb_address a;
    if (!ntb_address_decode(&a, address.toLatin1().constData()))
        return 0;
    int zeros = 0;
    while (zeros < 20 && a.ripe[zeros] == 0)
        ++zeros;
    return zeros;
}
} // namespace legacy
