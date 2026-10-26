#pragma once
// The parts of the fast vanity search (bm_vanity.cpp) that its tests reach.
//
// A Bitmessage address is made of two key pairs: RIPE = RIPEMD-160(SHA-512(04 X Y of the signing
// key, 04 X Y of the encryption key)). A search that makes the keys from a phrase pays two scalar
// multiplications for every address it looks at. This one keeps the signing key and steps the
// encryption key by one: the next public key is the last one plus G, a point addition, and a
// thousand of those share one field inversion. What is left of an address is its two hashes.
//
// Nor is the address made to look at it. Every address of the kind searched for (version 4,
// stream 1, a RIPE of 20 bytes) is a number whose top 128 bits are 04 01 and the first 14 bytes
// of the RIPE, and the first characters of the address are the first base 58 digits of that
// number: one multiplication and a dozen digit steps give them. A match is made in full and
// checked with the code of namespace ec, which is OpenSSL's and shares nothing with the rest.
//
// Needs a compiler with unsigned __int128, and OpenSSL's SHA512_Transform and RIPEMD160_Transform.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace vanity {
using u64 = std::uint64_t;
using u128 = unsigned __int128;

// --- the field of secp256k1, p = 2^256 - 2^32 - 977 ---------------------------------------------

struct Fe {
    u64 v[4]; // little-endian limbs; always below p
};

constexpr u64 kFold = 0x1000003D1ULL;            // 2^256 mod p
constexpr u64 kPrimeLow = 0xFFFFFFFEFFFFFC2FULL; // p's lowest limb; the others are all ones

inline bool atLeastP(const Fe &a) {
    return a.v[3] == ~0ULL && a.v[2] == ~0ULL && a.v[1] == ~0ULL && a.v[0] >= kPrimeLow;
}
// a - p for an a that is at least p: a + (2^256 - p), with the carry out dropped.
inline void takeP(Fe &a) {
    u128 t = (u128)a.v[0] + kFold;
    a.v[0] = (u64)t;
    for (int i = 1; i < 4; ++i) {
        t = (t >> 64) + a.v[i];
        a.v[i] = (u64)t;
    }
}
inline bool feIsZero(const Fe &a) {
    return (a.v[0] | a.v[1] | a.v[2] | a.v[3]) == 0;
}
inline bool feEqual(const Fe &a, const Fe &b) {
    return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2] && a.v[3] == b.v[3];
}

inline Fe feAdd(const Fe &a, const Fe &b) {
    Fe r;
    u128 t = 0;
    for (int i = 0; i < 4; ++i) {
        t += (u128)a.v[i] + b.v[i];
        r.v[i] = (u64)t;
        t >>= 64;
    }
    if (t) {
        // The sum is r + 2^256, which is r + kFold: r is small enough that this stays below p.
        u128 c = kFold;
        for (int i = 0; i < 4; ++i) {
            c += r.v[i];
            r.v[i] = (u64)c;
            c >>= 64;
        }
    } else if (atLeastP(r)) {
        takeP(r);
    }
    return r;
}

inline Fe feSub(const Fe &a, const Fe &b) {
    Fe r;
    u64 borrow = 0;
    for (int i = 0; i < 4; ++i) {
        const u128 d = (u128)a.v[i] - b.v[i] - borrow;
        r.v[i] = (u64)d;
        borrow = (u64)(d >> 64) & 1;
    }
    if (borrow) {
        // r is a - b + 2^256 and the answer is a - b + p, which is r - kFold: r is above kFold.
        u64 take = kFold;
        for (int i = 0; i < 4; ++i) {
            const u128 d = (u128)r.v[i] - take;
            r.v[i] = (u64)d;
            take = (u64)(d >> 64) & 1;
        }
    }
    return r;
}

inline Fe feNeg(const Fe &a) {
    return feSub(Fe{{0, 0, 0, 0}}, a);
}

// The 512-bit product of two numbers below 2^256, folded back with 2^256 = kFold.
inline Fe feReduce(const u64 t[8]) {
    u64 m[5];
    u128 c = 0;
    for (int i = 0; i < 4; ++i) {
        c += (u128)t[4 + i] * kFold;
        m[i] = (u64)c;
        c >>= 64;
    }
    m[4] = (u64)c; // below 2^34
    u64 w[5];
    u128 s = 0;
    for (int i = 0; i < 4; ++i) {
        s += (u128)t[i] + m[i];
        w[i] = (u64)s;
        s >>= 64;
    }
    w[4] = (u64)s + m[4];
    // w[4] * kFold is below 2^68; add it to the low 256 bits.
    const u128 f = (u128)w[4] * kFold;
    Fe r;
    s = (u128)w[0] + (u64)f;
    r.v[0] = (u64)s;
    s = (s >> 64) + w[1] + (u64)(f >> 64);
    r.v[1] = (u64)s;
    s = (s >> 64) + w[2];
    r.v[2] = (u64)s;
    s = (s >> 64) + w[3];
    r.v[3] = (u64)s;
    if (s >> 64) {
        // A carry out of bit 255: the value is r + 2^256 = r + kFold, and r is tiny now.
        u128 k = kFold;
        for (int i = 0; i < 4; ++i) {
            k += r.v[i];
            r.v[i] = (u64)k;
            k >>= 64;
        }
    }
    if (atLeastP(r))
        takeP(r);
    return r;
}

inline Fe feMul(const Fe &a, const Fe &b) {
    u64 t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        u128 carry = 0;
        for (int j = 0; j < 4; ++j) {
            carry += (u128)a.v[i] * b.v[j] + t[i + j];
            t[i + j] = (u64)carry;
            carry >>= 64;
        }
        t[i + 4] = (u64)carry;
    }
    return feReduce(t);
}

inline Fe feSqr(const Fe &a) {
    return feMul(a, a);
}
inline Fe feSqrN(Fe a, int n) {
    while (n-- > 0)
        a = feSqr(a);
    return a;
}

// a^(p-2), by the chain of 255 squarings and 15 multiplications; the inverse of zero is zero.
inline Fe feInv(const Fe &a) {
    const Fe x2 = feMul(feSqr(a), a);
    const Fe x3 = feMul(feSqr(x2), a);
    const Fe x6 = feMul(feSqrN(x3, 3), x3);
    const Fe x9 = feMul(feSqrN(x6, 3), x3);
    const Fe x11 = feMul(feSqrN(x9, 2), x2);
    const Fe x22 = feMul(feSqrN(x11, 11), x11);
    const Fe x44 = feMul(feSqrN(x22, 22), x22);
    const Fe x88 = feMul(feSqrN(x44, 44), x44);
    const Fe x176 = feMul(feSqrN(x88, 88), x88);
    const Fe x220 = feMul(feSqrN(x176, 44), x44);
    const Fe x223 = feMul(feSqrN(x220, 3), x3);
    Fe t = feMul(feSqrN(x223, 23), x22);
    t = feMul(feSqrN(t, 5), a);
    t = feMul(feSqrN(t, 3), x2);
    return feMul(feSqrN(t, 2), a);
}

inline u64 loadBE(const std::uint8_t *p) {
    u64 w;
    std::memcpy(&w, p, 8);
    return __builtin_bswap64(w);
}
inline void storeBE(std::uint8_t *p, u64 w) {
    w = __builtin_bswap64(w);
    std::memcpy(p, &w, 8);
}

// 32 big-endian bytes; a number at or above p is taken modulo p.
inline Fe feFromBytes(const std::uint8_t in[32]) {
    Fe r{{loadBE(in + 24), loadBE(in + 16), loadBE(in + 8), loadBE(in)}};
    if (atLeastP(r))
        takeP(r); // below 2^256, so below 2p
    return r;
}
inline void feToBytes(const Fe &a, std::uint8_t out[32]) {
    storeBE(out, a.v[3]);
    storeBE(out + 8, a.v[2]);
    storeBE(out + 16, a.v[1]);
    storeBE(out + 24, a.v[0]);
}

// --- the curve ----------------------------------------------------------------------------------

struct Affine {
    Fe x, y;
};

namespace detail {
inline Fe feOfHex(const char (&hex)[65]) {
    std::uint8_t b[32];
    for (int i = 0; i < 32; ++i) {
        const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        b[i] = std::uint8_t(digit(hex[2 * i]) << 4 | digit(hex[2 * i + 1]));
    }
    return feFromBytes(b);
}
// a + b on y^2 = x^3 + 7; false when the sum is the point at infinity.
inline bool addAffine(const Affine &a, const Affine &b, Affine &out) {
    Fe lambda;
    if (feEqual(a.x, b.x)) {
        if (!feEqual(a.y, b.y) || feIsZero(a.y))
            return false;
        const Fe xx = feSqr(a.x);
        lambda = feMul(feAdd(feAdd(xx, xx), xx), feInv(feAdd(a.y, a.y)));
    } else {
        lambda = feMul(feSub(b.y, a.y), feInv(feSub(b.x, a.x)));
    }
    const Fe x = feSub(feSub(feSqr(lambda), a.x), b.x);
    out = {x, feSub(feMul(lambda, feSub(a.x, x)), a.y)};
    return true;
}
} // namespace detail

// The points around a centre, c + j*G for j from -kHalf to kHalf, and the step from one window to
// the next. The multiples j*G are made once; each window then costs three multiplications and a
// squaring per point, and one inversion.
class Stepper {
  public:
    static constexpr int kHalf = 512;
    static constexpr int kWidth = 2 * kHalf + 1;

    Stepper() {
        const Affine g{
            detail::feOfHex("79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798"),
            detail::feOfHex("483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8")};
        Affine at = g;
        for (int j = 0; j < kHalf; ++j) {
            gx_[j] = at.x;
            gy_[j] = at.y;
            ngy_[j] = feNeg(at.y);
            if (!detail::addAffine(at, g, at)) // the next multiple
                throw std::runtime_error("the multiples of G ran out");
        }
        // The step from one window's centre to the next, kWidth*G.
        Affine step = g;
        for (int j = 1; j < kWidth; ++j)
            if (!detail::addAffine(step, g, step))
                throw std::runtime_error("the multiples of G ran out");
        step_ = step;
    }

    // Calls f(j, x, y) for the centre (j = 0) and for c + j*G with j from -kHalf to kHalf. False,
    // and no call, when a point of the window is the centre's opposite and the sum is at infinity
    // (it cannot happen to a random key).
    template <class F> bool window(const Affine &c, F &&f) const {
        Fe delta[kHalf], pre[kHalf];
        for (int j = 0; j < kHalf; ++j)
            delta[j] = feSub(gx_[j], c.x);
        // One inversion for all of them: the inverse of the running product, unwound.
        pre[0] = delta[0];
        for (int j = 1; j < kHalf; ++j)
            pre[j] = feMul(pre[j - 1], delta[j]);
        if (feIsZero(pre[kHalf - 1]))
            return false;
        Fe inv = feInv(pre[kHalf - 1]);
        for (int j = kHalf - 1; j > 0; --j) {
            const Fe invJ = feMul(inv, pre[j - 1]);
            inv = feMul(inv, delta[j]);
            delta[j] = invJ;
        }
        delta[0] = inv;

        f(0, c.x, c.y);
        for (int j = 0; j < kHalf; ++j) {
            const Fe &gx = gx_[j];
            // c + (j+1)G, and then c - (j+1)G, which has the same denominator.
            Fe lambda = feMul(feSub(gy_[j], c.y), delta[j]);
            Fe x = feSub(feSub(feSqr(lambda), c.x), gx);
            Fe y = feSub(feMul(lambda, feSub(c.x, x)), c.y);
            f(j + 1, x, y);
            lambda = feMul(feSub(ngy_[j], c.y), delta[j]);
            x = feSub(feSub(feSqr(lambda), c.x), gx);
            y = feSub(feMul(lambda, feSub(c.x, x)), c.y);
            f(-(j + 1), x, y);
        }
        return true;
    }

    // The centre of the next window: c + kWidth*G.
    Affine advance(const Affine &c) const {
        Affine out;
        if (!detail::addAffine(c, step_, out))
            throw std::runtime_error("a window ended at infinity");
        return out;
    }

  private:
    Fe gx_[kHalf], gy_[kHalf], ngy_[kHalf]; // j*G for j from 1, and the negatives of the y's
    Affine step_;
};

// --- OpenSSL's curve: start points, and the checks that trust nothing above ---------------------

namespace ec {
// The base 58 of a Bitmessage address.
inline constexpr char kAlphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

class Curve {
  public:
    Curve()
        : group_(EC_GROUP_new_by_curve_name(NID_secp256k1)), ctx_(BN_CTX_new()), order_(BN_new()) {
        if (!group_ || !ctx_ || !order_ || !EC_GROUP_get_order(group_, order_, ctx_))
            throw std::runtime_error("OpenSSL has no secp256k1");
    }
    ~Curve() {
        BN_free(order_);
        BN_CTX_free(ctx_);
        EC_GROUP_free(group_);
    }
    Curve(const Curve &) = delete;
    Curve &operator=(const Curve &) = delete;

    // The order of the curve, 32 bytes.
    void order(std::uint8_t out[32]) const {
        BN_bn2binpad(order_, out, 32);
    }
    // A private key is 1 up to the order less 1.
    bool valid(const std::uint8_t priv[32]) const {
        BIGNUM *k = BN_bin2bn(priv, 32, nullptr);
        const bool ok = k && !BN_is_zero(k) && BN_cmp(k, order_) < 0;
        BN_clear_free(k);
        return ok;
    }
    // The 65 bytes of the public key (04 X Y); false for a key that is not valid.
    bool publicKey(const std::uint8_t priv[32], std::uint8_t out[65]) const {
        if (!valid(priv))
            return false;
        BIGNUM *k = BN_bin2bn(priv, 32, nullptr);
        EC_POINT *point = EC_POINT_new(group_);
        const bool ok =
            point && EC_POINT_mul(group_, point, k, nullptr, nullptr, ctx_) == 1 &&
            EC_POINT_point2oct(group_, point, POINT_CONVERSION_UNCOMPRESSED, out, 65, ctx_) == 65;
        EC_POINT_free(point);
        BN_clear_free(k);
        return ok;
    }
    // (base + delta) mod the order.
    void add(const std::uint8_t base[32], std::uint64_t delta, std::uint8_t out[32]) const {
        BIGNUM *k = BN_bin2bn(base, 32, nullptr);
        BN_add_word(k, delta);
        BN_mod(k, k, order_, ctx_);
        BN_bn2binpad(k, out, 32);
        BN_clear_free(k);
    }

  private:
    EC_GROUP *group_;
    BN_CTX *ctx_;
    BIGNUM *order_;
};
inline const Curve &curve() {
    static thread_local const Curve c;
    return c;
}

inline std::string base58(std::vector<std::uint8_t> number) {
    std::string out;
    size_t start = 0;
    while (start < number.size() && number[start] == 0)
        ++start;
    while (start < number.size()) {
        int rem = 0;
        for (size_t i = start; i < number.size(); ++i) {
            const int cur = rem * 256 + number[i];
            number[i] = std::uint8_t(cur / 58);
            rem = cur % 58;
        }
        out += kAlphabet[rem];
        while (start < number.size() && number[start] == 0)
            ++start;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// The address of a RIPE, version 4 in stream 1: the leading zero bytes of the RIPE are left out.
inline std::string addressOfRipe(const std::uint8_t ripe[20]) {
    int zeros = 0;
    while (zeros < 20 && ripe[zeros] == 0)
        ++zeros;
    std::vector<std::uint8_t> data{4, 1};
    data.insert(data.end(), ripe + zeros, ripe + 20);
    std::uint8_t first[64], second[64];
    SHA512(data.data(), data.size(), first);
    SHA512(first, 64, second);
    data.insert(data.end(), second, second + 4);
    return "BM-" + base58(data);
}

// The address of two private keys, made with nothing but OpenSSL's calls; false for a bad key.
inline bool address(const std::uint8_t signing[32], const std::uint8_t encryption[32],
                    std::string &out) {
    std::uint8_t keys[130], sha[64], ripe[20];
    if (!curve().publicKey(signing, keys) || !curve().publicKey(encryption, keys + 65))
        return false;
    SHA512(keys, 130, sha);
    RIPEMD160(sha, 64, ripe);
    out = addressOfRipe(ripe);
    return true;
}
} // namespace ec

inline std::string addressOfRipe(const std::uint8_t ripe[20]) {
    return ec::addressOfRipe(ripe);
}

// --- what is looked for -------------------------------------------------------------------------

class Pattern {
  public:
    static constexpr int kMaxDigits = 16; // how far into an address a match can be read

    // The word, at a place from posMin to posMax characters after "BM-". Any case of a letter is
    // taken unless exact; the case the word is typed in is what a "letter as typed" means. False,
    // with the reason, for a word the base 58 of an address cannot spell.
    bool set(const std::string &word, int posMin, int posMax, bool exact, std::string *problem) {
        const auto refuse = [&](const std::string &why) {
            if (problem)
                *problem = why;
            return false;
        };
        if (word.empty())
            return refuse("The word is empty");
        if (posMin < 0 || posMin > posMax)
            return refuse("The places must satisfy 0 <= pos-min <= pos-max");
        if (posMax + int(word.size()) > kMaxDigits)
            return refuse("The word is too long: only the first " + std::to_string(kMaxDigits) +
                          " characters after BM- can be looked at, and pos-max + the word's "
                          "length must fit");
        std::array<u64, kMaxDigits> mask{};
        for (size_t i = 0; i < word.size(); ++i) {
            const char c = word[i];
            for (const char variant : {c, exact ? c : char(std::tolower((unsigned char)c)),
                                       exact ? c : char(std::toupper((unsigned char)c))})
                for (int d = 0; d < 58; ++d)
                    if (ec::kAlphabet[d] == variant)
                        mask[i] |= 1ULL << d;
            if (!mask[i])
                return refuse(std::string("'") + c +
                              "' is not in base 58, which has no 0, O, I "
                              "or l, and so no address has it");
        }
        word_ = word;
        posMin_ = posMin;
        posMax_ = posMax;
        exact_ = exact;
        mask_ = mask;
        digits_ = posMax + int(word.size());
        return true;
    }
    const std::string &word() const {
        return word_;
    }
    int posMin() const {
        return posMin_;
    }
    int posMax() const {
        return posMax_;
    }

    // The place the word is at, in the address of a RIPE of 20 bytes that does not start with a
    // zero byte, or -1. Reads the digits off the top of the number the address is, as the header
    // says; nothing is made in full.
    int match(const std::uint8_t ripe[20]) const {
        // 04 01 and the first 14 bytes of the RIPE: the top 128 bits of the address's number.
        const u128 top =
            ((u128)((0x0401ULL << 48) | (loadBE(ripe) >> 16)) << 64) | loadBE(ripe + 6);
        // The number over 58^35 as a fraction of 2^128: top * beta, beta = 7 + kBetaFraction/2^128.
        const u128 f = top * 7 + mulHigh(top, ((u128)kBetaHigh << 64) | kBetaLow);
        u64 high = (u64)(f >> 64), low = (u64)f;
        std::uint8_t digit[kMaxDigits];
        for (int i = 0; i < digits_; ++i) {
            // Times 58: what spills over the top is the next digit.
            const u128 l = (u128)low * 58;
            low = (u64)l;
            const u128 h = (u128)high * 58 + (u64)(l >> 64);
            high = (u64)h;
            digit[i] = std::uint8_t(h >> 64);
        }
        for (int at = posMin_; at <= posMax_; ++at) {
            bool same = true;
            for (size_t i = 0; i < word_.size() && same; ++i)
                same = (mask_[i] >> digit[at + i]) & 1;
            if (same)
                return at;
        }
        return -1;
    }

    // The place of the word in an address's text, or -1: the plain way, to check a match.
    int find(const std::string &address) const {
        for (int at = posMin_; at <= posMax_; ++at) {
            const size_t from = 3 + size_t(at);
            if (from + word_.size() > address.size())
                break;
            bool same = true;
            for (size_t i = 0; i < word_.size() && same; ++i) {
                const unsigned char a = address[from + i], b = word_[i];
                same = exact_ ? a == b : std::tolower(a) == std::tolower(b);
            }
            if (same)
                return at;
        }
        return -1;
    }

    // How many of the word's letters an address has in the case they were typed in.
    int exactLetters(const std::string &address, int place) const {
        if (place < 0 || 3 + size_t(place) + word_.size() > address.size())
            return 0;
        int n = 0;
        for (size_t i = 0; i < word_.size(); ++i)
            n += address[3 + size_t(place) + i] == word_[i];
        return n;
    }

  private:
    // top * beta needs the high half of a product of two 128-bit numbers.
    static u128 mulHigh(u128 a, u128 b) {
        const u64 a0 = (u64)a, a1 = (u64)(a >> 64), b0 = (u64)b, b1 = (u64)(b >> 64);
        const u128 p00 = (u128)a0 * b0, p01 = (u128)a0 * b1, p10 = (u128)a1 * b0,
                   p11 = (u128)a1 * b1;
        const u128 mid = (p00 >> 64) + (u64)p01 + (u64)p10;
        return p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
    }
    // floor(2^208 * 2^128 / 58^35) = 7 * 2^128 + this: the factor that turns the top 128 bits of an
    // address's number (04 01, 14 bytes of RIPE) into that number as a fraction of 58^35.
    static constexpr u64 kBetaHigh = 0xd6c71e1e93d60c15ULL;
    static constexpr u64 kBetaLow = 0x0f06e81365b76714ULL;

    std::string word_;
    int posMin_ = 0, posMax_ = 0, digits_ = 0;
    bool exact_ = false;
    std::array<u64, kMaxDigits> mask_{}; // the digits each letter may be
};

// --- the two hashes -----------------------------------------------------------------------------

// RIPE = RIPEMD-160(SHA-512(signing public key, encryption public key)) for a signing key that
// stays and an encryption key that changes. The 130 bytes are two SHA-512 blocks and the 64 of
// the digest two RIPEMD-160 blocks, hashed with the transforms alone: no copying, no padding work.
class PairHasher {
  public:
    explicit PairHasher(const std::uint8_t signingPublic[65]) {
        std::memset(message_, 0, sizeof message_);
        std::memcpy(message_, signingPublic, 65);
        message_[65] = 0x04;
        message_[130] = 0x80;
        message_[254] = 0x04; // 130 bytes are 1040 bits, 0x0410, at the end of the second block
        message_[255] = 0x10;
        std::memset(padding_, 0, sizeof padding_);
        padding_[0] = 0x80;
        padding_[57] = 0x02; // 64 bytes are 512 bits, little-endian
        SHA512_Init(&sha_);
        std::memcpy(shaStart_, sha_.h, sizeof shaStart_);
        RIPEMD160_Init(&ripemd_);
        ripemdStart_[0] = ripemd_.A;
        ripemdStart_[1] = ripemd_.B;
        ripemdStart_[2] = ripemd_.C;
        ripemdStart_[3] = ripemd_.D;
        ripemdStart_[4] = ripemd_.E;
    }

    void ripe(const Fe &x, const Fe &y, std::uint8_t out[20]) {
        feToBytes(x, message_ + 66);
        feToBytes(y, message_ + 98);
        std::memcpy(sha_.h, shaStart_, sizeof shaStart_);
        SHA512_Transform(&sha_, message_);
        SHA512_Transform(&sha_, message_ + 128);
        for (int i = 0; i < 8; ++i)
            storeBE(digest_ + 8 * i, sha_.h[i]);
        ripemd_.A = ripemdStart_[0];
        ripemd_.B = ripemdStart_[1];
        ripemd_.C = ripemdStart_[2];
        ripemd_.D = ripemdStart_[3];
        ripemd_.E = ripemdStart_[4];
        RIPEMD160_Transform(&ripemd_, digest_);
        RIPEMD160_Transform(&ripemd_, padding_);
        const unsigned words[5] = {ripemd_.A, ripemd_.B, ripemd_.C, ripemd_.D, ripemd_.E};
        for (int i = 0; i < 5; ++i) { // little-endian words
            for (int b = 0; b < 4; ++b)
                out[4 * i + b] = std::uint8_t(words[i] >> (8 * b));
        }
    }

  private:
    alignas(16) std::uint8_t message_[256];
    alignas(16) std::uint8_t digest_[64];
    alignas(16) std::uint8_t padding_[64];
    SHA512_CTX sha_;
    RIPEMD160_CTX ripemd_;
    SHA_LONG64 shaStart_[8];
    unsigned ripemdStart_[5];
};

// --- one thread's walk --------------------------------------------------------------------------

// Looks through the encryption keys start + 0, 1, 2, ... for a signing key that stays, a window
// at a time, and says which of them have the word in their address.
class Walker {
  public:
    static constexpr int kWidth = Stepper::kWidth;

    Walker(const Pattern &pattern, const Stepper &stepper, const std::uint8_t signing[32],
           const std::uint8_t start[32])
        : pattern_(pattern), stepper_(stepper), hasher_(publicOf(signing).data()) {
        std::uint8_t centre[32];
        ec::curve().add(start, Stepper::kHalf, centre);
        const auto pub = publicOf(centre);
        centre_ = {feFromBytes(pub.data() + 1), feFromBytes(pub.data() + 33)};
    }

    // The next kWidth keys. hit(delta, place) for each whose address has the word: delta is how
    // far its encryption key is above the start.
    template <class Hit> void next(Hit &&hit) {
        stepper_.window(centre_, [&](int j, const Fe &x, const Fe &y) {
            std::uint8_t ripe[20];
            hasher_.ripe(x, y, ripe);
            if (ripe[0] == 0)
                return; // an address of another length, whose digits these are not
            const int place = pattern_.match(ripe);
            if (place >= 0)
                hit(centreAt_ + std::uint64_t(std::int64_t(j)), place);
        });
        centre_ = stepper_.advance(centre_);
        centreAt_ += kWidth;
    }

  private:
    static std::array<std::uint8_t, 65> publicOf(const std::uint8_t priv[32]) {
        std::array<std::uint8_t, 65> pub;
        if (!ec::curve().publicKey(priv, pub.data()))
            throw std::runtime_error("not a private key");
        return pub;
    }

    const Pattern &pattern_;
    const Stepper &stepper_;
    PairHasher hasher_;
    Affine centre_;
    std::uint64_t centreAt_ = Stepper::kHalf; // the centre's key is start + this
};
} // namespace vanity

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
