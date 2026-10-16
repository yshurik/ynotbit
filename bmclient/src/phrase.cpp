#include <bmc/bytes.h>
#include <bmc/phrase.h>
#include <bmc/protocol.h>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <secp256k1.h>
extern "C" {
#include "config.h"
#include "ntb-address.h"
}

namespace bmc::phrase {
struct Context::Impl {
    secp256k1_context *p = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    ~Impl() {
        secp256k1_context_destroy(p);
    }
};
Context::Context() : p_(new Impl) {}
Context::~Context() = default;

int Context::pairZeroBytes(const unsigned char sign[32], const unsigned char enc[32]) const {
    unsigned char keys[130];
    secp256k1_pubkey point;
    size_t length = 65;
    const secp256k1_context *ctx = p_->p;
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

void key(std::string_view phrase, uint64_t nonce, unsigned char out[32]) {
    Bytes input = bytesOf(phrase);
    putVarInt(input, nonce);
    unsigned char digest[64];
    SHA512(input.data(), input.size(), digest);
    std::memcpy(out, digest, 32);
    OPENSSL_cleanse(digest, sizeof digest);
    OPENSSL_cleanse(input.data(), input.size());
}

Identity fromKeys(const unsigned char sign[32], const unsigned char enc[32]) {
    return Protocol::fromPrivate({}, ByteView(sign, 32), ByteView(enc, 32), 4, 1, false);
}

Identity identity(std::string_view phrase, std::string_view label, uint64_t pair) {
    unsigned char sign[32], enc[32];
    key(phrase, 2 * pair, sign);
    key(phrase, 2 * pair + 1, enc);
    auto id = fromKeys(sign, enc);
    id.label = std::string(label);
    OPENSSL_cleanse(sign, 32);
    OPENSSL_cleanse(enc, 32);
    return id;
}

std::optional<uint64_t> firstPair(std::string_view phrase, int zeroBytes, uint64_t limit) {
    Context ctx;
    unsigned char sign[32], enc[32];
    std::optional<uint64_t> found;
    for (uint64_t pair = 0; pair < limit && !found; ++pair) {
        key(phrase, 2 * pair, sign);
        key(phrase, 2 * pair + 1, enc);
        if (ctx.pairZeroBytes(sign, enc) >= zeroBytes)
            found = pair;
    }
    OPENSSL_cleanse(sign, 32);
    OPENSSL_cleanse(enc, 32);
    return found;
}

int zeroBytes(std::string_view address) {
    ntb_address a;
    const std::string text(address);
    if (!ntb_address_decode(&a, text.c_str()))
        return 0;
    int zeros = 0;
    while (zeros < 20 && a.ripe[zeros] == 0)
        ++zeros;
    return zeros;
}
} // namespace bmc::phrase
