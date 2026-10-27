#include "avx512.h"

#if defined(__x86_64__) && defined(__AVX512F__) && defined(__AVX512BW__) &&                        \
    defined(__AVX512DQ__) && defined(__AVX512VL__)
#define VANITY_HAVE_AVX512 1
#include <immintrin.h>
#endif

namespace vanity::avx512 {
#ifdef VANITY_HAVE_AVX512
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using V = __m512i; // eight 64-bit lanes for SHA-512, sixteen 32-bit lanes for RIPEMD-160

// --- SHA-512 on eight lanes
// -----------------------------------------------------------------------

// The cube roots' fractions of the first 80 primes; the square roots' of the first 8 are the start.
alignas(64) constexpr u64 kShaK[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
constexpr u64 kShaStart[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                              0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                              0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

inline V add(V a, V b) {
    return _mm512_add_epi64(a, b);
}
template <int N> inline V ror(V x) {
    return _mm512_ror_epi64(x, N);
}
inline V xor3(V a, V b, V c) {
    return _mm512_ternarylogic_epi64(a, b, c, 0x96);
}
inline V bigSigma0(V x) {
    return xor3(ror<28>(x), ror<34>(x), ror<39>(x));
}
inline V bigSigma1(V x) {
    return xor3(ror<14>(x), ror<18>(x), ror<41>(x));
}
inline V smallSigma0(V x) {
    return xor3(ror<1>(x), ror<8>(x), _mm512_srli_epi64(x, 7));
}
inline V smallSigma1(V x) {
    return xor3(ror<19>(x), ror<61>(x), _mm512_srli_epi64(x, 6));
}
inline V choose(V e, V f, V g) { // e ? f : g
    return _mm512_ternarylogic_epi64(e, f, g, 0xCA);
}
inline V majority(V a, V b, V c) {
    return _mm512_ternarylogic_epi64(a, b, c, 0xE8);
}

// One round, with the variables named as the round's place in a group of eight turns them: the new
// e is d + T1, which goes where d was, and the new a is T1 + T2, which goes where h was.
#define VANITY_SHA_ROUND(a, b, c, d, e, f, g, h, t)                                                \
    {                                                                                              \
        const V t1 = add(add(h, bigSigma1(e)),                                                     \
                         add(choose(e, f, g), add(_mm512_set1_epi64((long long)kShaK[t]), w[t]))); \
        d = add(d, t1);                                                                            \
        h = add(t1, add(bigSigma0(a), majority(a, b, c)));                                         \
    }

// The state, with a block of sixteen message words added in.
inline void shaCompress(V s[8], const V in[16]) {
    V w[80];
    for (int t = 0; t < 16; ++t)
        w[t] = in[t];
    for (int t = 16; t < 80; ++t)
        w[t] = add(add(smallSigma1(w[t - 2]), w[t - 7]), add(smallSigma0(w[t - 15]), w[t - 16]));
    V a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int t = 0; t < 80; t += 8) {
        VANITY_SHA_ROUND(a, b, c, d, e, f, g, h, t)
        VANITY_SHA_ROUND(h, a, b, c, d, e, f, g, t + 1)
        VANITY_SHA_ROUND(g, h, a, b, c, d, e, f, t + 2)
        VANITY_SHA_ROUND(f, g, h, a, b, c, d, e, t + 3)
        VANITY_SHA_ROUND(e, f, g, h, a, b, c, d, t + 4)
        VANITY_SHA_ROUND(d, e, f, g, h, a, b, c, t + 5)
        VANITY_SHA_ROUND(c, d, e, f, g, h, a, b, t + 6)
        VANITY_SHA_ROUND(b, c, d, e, f, g, h, a, t + 7)
    }
    s[0] = add(s[0], a);
    s[1] = add(s[1], b);
    s[2] = add(s[2], c);
    s[3] = add(s[3], d);
    s[4] = add(s[4], e);
    s[5] = add(s[5], f);
    s[6] = add(s[6], g);
    s[7] = add(s[7], h);
}

inline V bytesReversedIn32() {
    return _mm512_broadcast_i32x4(
        _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}

// The two blocks of the message that SHA-512 is given for eight encryption keys (lanes at to at +
// 7): 04 X Y of the signing key and 04 X Y of the encryption key, 130 bytes. Its words are the
// bytes shifted by the one byte that the 04 of the encryption key makes, so each is made of the end
// of one limb and the start of the next.
inline void keyMessage(const u64 first[8], u8 last, const u64 xs[4][kLanes],
                       const u64 ys[4][kLanes], int at, V block1[16], V block2[16]) {
    const V low16 = _mm512_set1_epi64(0xFFFF);
    // The first eight bytes of x, big-endian, are the number that its highest limb is: the limbs
    // are the big-endian words, from the highest down. Then y's.
    const auto load = [&](const u64 *p) { return _mm512_loadu_si512(p); };
    const V x0 = load(&xs[3][at]), x1 = load(&xs[2][at]), x2 = load(&xs[1][at]),
            x3 = load(&xs[0][at]);
    const V y0 = load(&ys[3][at]), y1 = load(&ys[2][at]), y2 = load(&ys[1][at]),
            y3 = load(&ys[0][at]);
    // The word that has the last two bytes of one word and the first six of the next.
    const auto join = [&](V high, V low) {
        return _mm512_or_si512(_mm512_slli_epi64(_mm512_and_si512(high, low16), 48),
                               _mm512_srli_epi64(low, 16));
    };
    for (int i = 0; i < 8; ++i)
        block1[i] = _mm512_set1_epi64((long long)first[i]);
    block1[8] = _mm512_or_si512(_mm512_set1_epi64((long long)((u64(last) << 56) | (0x04ULL << 48))),
                                _mm512_srli_epi64(x0, 16));
    block1[9] = join(x0, x1);
    block1[10] = join(x1, x2);
    block1[11] = join(x2, x3);
    block1[12] = join(x3, y0);
    block1[13] = join(y0, y1);
    block1[14] = join(y1, y2);
    block1[15] = join(y2, y3);
    // The second block: the last two bytes of y, the 80 that ends the message, zeros, and the
    // length of 130 bytes in bits, 0x410, as a 128-bit number at the end.
    for (int i = 0; i < 16; ++i)
        block2[i] = _mm512_setzero_si512();
    block2[0] = _mm512_or_si512(_mm512_slli_epi64(_mm512_and_si512(y3, low16), 48),
                                _mm512_set1_epi64((long long)(0x80ULL << 40)));
    block2[15] = _mm512_set1_epi64(0x410);
}

// SHA-512 of that message, for eight keys.
inline void shaOfKeys(const u64 first[8], u8 last, const u64 xs[4][kLanes], const u64 ys[4][kLanes],
                      int at, V digest[8]) {
    V block1[16], block2[16];
    keyMessage(first, last, xs, ys, at, block1, block2);
    for (int i = 0; i < 8; ++i)
        digest[i] = _mm512_set1_epi64((long long)kShaStart[i]);
    shaCompress(digest, block1);
    shaCompress(digest, block2);
}

// --- RIPEMD-160 on sixteen lanes
// --------------------------------------------------------------------

constexpr u32 kRmdStart[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
// Which message word each of the 80 steps of the left line, and of the right line, takes.
constexpr u8 kLeftWord[80] = {0, 1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
                              7, 4,  13, 1,  10, 6,  15, 3,  12, 0, 9,  5,  2,  14, 11, 8,
                              3, 10, 14, 4,  9,  15, 8,  1,  2,  7, 0,  6,  13, 11, 5,  12,
                              1, 9,  11, 10, 0,  8,  12, 4,  13, 3, 7,  15, 14, 5,  6,  2,
                              4, 0,  5,  9,  7,  12, 2,  10, 14, 1, 3,  8,  11, 6,  15, 13};
constexpr u8 kRightWord[80] = {5,  14, 7,  0, 9, 2,  11, 4,  13, 6,  15, 8,  1,  10, 3,  12,
                               6,  11, 3,  7, 0, 13, 5,  10, 14, 15, 8,  12, 4,  9,  1,  2,
                               15, 5,  1,  3, 7, 14, 6,  9,  11, 8,  12, 2,  10, 0,  4,  13,
                               8,  6,  4,  1, 3, 11, 15, 0,  5,  12, 2,  13, 9,  7,  10, 14,
                               12, 15, 10, 4, 1, 5,  8,  7,  6,  2,  13, 14, 0,  3,  9,  11};
// How far each step rotates.
constexpr u8 kLeftTurn[80] = {11, 14, 15, 12, 5,  8,  7,  9,  11, 13, 14, 15, 6,  7,  9,  8,
                              7,  6,  8,  13, 11, 9,  7,  15, 7,  12, 15, 9,  11, 7,  13, 12,
                              11, 13, 6,  7,  14, 9,  13, 15, 14, 8,  13, 6,  5,  12, 7,  5,
                              11, 12, 14, 15, 14, 15, 9,  8,  9,  14, 5,  6,  8,  6,  5,  12,
                              9,  15, 5,  11, 6,  8,  13, 12, 5,  12, 13, 14, 11, 8,  5,  6};
constexpr u8 kRightTurn[80] = {8,  9,  9,  11, 13, 15, 15, 5,  7,  7,  8,  11, 14, 14, 12, 6,
                               9,  13, 15, 7,  12, 8,  9,  11, 7,  7,  12, 7,  6,  15, 13, 11,
                               9,  7,  15, 11, 8,  6,  6,  14, 12, 13, 5,  14, 13, 13, 7,  5,
                               15, 5,  8,  11, 14, 14, 6,  14, 6,  9,  12, 9,  12, 5,  15, 8,
                               8,  5,  12, 9,  12, 5,  14, 6,  8,  13, 6,  5,  15, 13, 11, 11};
constexpr u32 kLeftAdd[5] = {0x00000000u, 0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xA953FD4Eu};
constexpr u32 kRightAdd[5] = {0x50A28BE6u, 0x5C4DD124u, 0x6D703EF3u, 0x7A6D76E9u, 0x00000000u};

inline V add32(V a, V b) {
    return _mm512_add_epi32(a, b);
}
// The five functions of the rounds, as the truth tables of a ternary logic instruction:
// x^y^z, x?y:z, (x|~y)^z, z?x:y, x^(y|~z).
inline V rmdFunction(int round, V x, V y, V z) {
    switch (round) {
    case 0:
        return _mm512_ternarylogic_epi32(x, y, z, 0x96);
    case 1:
        return _mm512_ternarylogic_epi32(x, y, z, 0xCA);
    case 2:
        return _mm512_ternarylogic_epi32(x, y, z, 0x59);
    case 3:
        return _mm512_ternarylogic_epi32(x, y, z, 0xE4);
    default:
        return _mm512_ternarylogic_epi32(x, y, z, 0x2D);
    }
}
inline V rotl(V x, int turn) {
    return _mm512_rolv_epi32(x, _mm512_set1_epi32(turn));
}

// The state, with a block of sixteen message words added in: the left line and the right line, then
// the two put together.
inline void rmdCompress(V h[5], const V x[16]) {
    V al = h[0], bl = h[1], cl = h[2], dl = h[3], el = h[4];
    V ar = al, br = bl, cr = cl, dr = dl, er = el;
#pragma GCC unroll 5
    for (int round = 0; round < 5; ++round) {
#pragma GCC unroll 16
        for (int i = 0; i < 16; ++i) {
            const int j = round * 16 + i;
            V t = add32(add32(al, rmdFunction(round, bl, cl, dl)),
                        add32(x[kLeftWord[j]], _mm512_set1_epi32((int)kLeftAdd[round])));
            t = add32(rotl(t, kLeftTurn[j]), el);
            al = el;
            el = dl;
            dl = rotl(cl, 10);
            cl = bl;
            bl = t;
            V u = add32(add32(ar, rmdFunction(4 - round, br, cr, dr)),
                        add32(x[kRightWord[j]], _mm512_set1_epi32((int)kRightAdd[round])));
            u = add32(rotl(u, kRightTurn[j]), er);
            ar = er;
            er = dr;
            dr = rotl(cr, 10);
            cr = br;
            br = u;
        }
    }
    const V t = add32(add32(h[1], cl), dr);
    h[1] = add32(add32(h[2], dl), er);
    h[2] = add32(add32(h[3], el), ar);
    h[3] = add32(add32(h[4], al), br);
    h[4] = add32(add32(h[0], bl), cr);
    h[0] = t;
}

// The sixteen 32-bit message words RIPEMD-160 reads from a SHA-512 digest of its sixteen lanes (two
// groups of eight): the digest's bytes in order, four to a word, the first byte lowest.
inline void rmdWordsOfDigests(const V low[8], const V high[8], V x[16]) {
    const V reverse = bytesReversedIn32();
    const auto together = [](__m256i a, __m256i b) {
        return _mm512_inserti64x4(_mm512_castsi256_si512(a), b, 1);
    };
    for (int i = 0; i < 8; ++i) {
        const V upper = together(_mm512_cvtepi64_epi32(_mm512_srli_epi64(low[i], 32)),
                                 _mm512_cvtepi64_epi32(_mm512_srli_epi64(high[i], 32)));
        const V lower = together(_mm512_cvtepi64_epi32(low[i]), _mm512_cvtepi64_epi32(high[i]));
        x[2 * i] = _mm512_shuffle_epi8(upper, reverse);
        x[2 * i + 1] = _mm512_shuffle_epi8(lower, reverse);
    }
}

// --- reading an address off a RIPE
// -------------------------------------------------------------------

// 2^144 / 58^35: the address's number is about (04 01 and six bytes of RIPE) * 2^144 over 58^35 of
// the way to the next power of 58, a fraction whose first digits are the address's first
// characters.
constexpr double kScale = 0x1.f5b1c787a4f58p-62;
} // namespace

bool supported() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
           __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512vl");
}

BatchHasher::BatchHasher(const std::uint8_t signingPublic[65]) {
    for (int i = 0; i < 8; ++i) {
        u64 word = 0;
        for (int b = 0; b < 8; ++b)
            word = (word << 8) | signingPublic[8 * i + b];
        first_[i] = word;
    }
    last_ = signingPublic[64];
}

void BatchHasher::hash(const std::uint64_t xs[4][kLanes], const std::uint64_t ys[4][kLanes],
                       std::uint32_t words[4][kLanes]) const {
    V low[8], high[8];
    shaOfKeys(first_, last_, xs, ys, 0, low);
    shaOfKeys(first_, last_, xs, ys, 8, high);
    V x[16];
    rmdWordsOfDigests(low, high, x);
    V h[5];
    for (int i = 0; i < 5; ++i)
        h[i] = _mm512_set1_epi32((int)kRmdStart[i]);
    rmdCompress(h, x);
    // The second block: the 80 that ends the message, zeros, and its length, 512 bits.
    for (int i = 0; i < 16; ++i)
        x[i] = _mm512_setzero_si512();
    x[0] = _mm512_set1_epi32(0x80);
    x[14] = _mm512_set1_epi32(512);
    rmdCompress(h, x);
    for (int w = 0; w < 4; ++w)
        _mm512_storeu_si512(words[w], h[w]);
}

std::uint16_t BatchHasher::candidates(const std::uint32_t words[4][kLanes], int posMin, int posMax,
                                      const std::uint64_t *letters, int letterCount) const {
    const V reverse = bytesReversedIn32();
    // The RIPE's bytes in order, four to a word: byte 0 the highest.
    const V b0 = _mm512_shuffle_epi8(_mm512_loadu_si512(words[0]), reverse);
    const V b1 = _mm512_shuffle_epi8(_mm512_loadu_si512(words[1]), reverse);
    const __mmask16 notZero =
        _mm512_test_epi32_mask(_mm512_srli_epi32(b0, 24), _mm512_set1_epi32(0xFF));
    const auto half = [&](__m256i first, __m256i second) -> __mmask8 {
        // The top 64 bits of the address's number: 04 01 and RIPE bytes 0 to 5.
        const V a = _mm512_cvtepu32_epi64(first), b = _mm512_cvtepu32_epi64(second);
        const V six = _mm512_or_si512(_mm512_slli_epi64(a, 16), _mm512_srli_epi64(b, 16));
        const V top = _mm512_or_si512(six, _mm512_set1_epi64((long long)(0x0401ULL << 48)));
        __m512d fraction = _mm512_mul_pd(_mm512_cvtepu64_pd(top), _mm512_set1_pd(kScale));
        V digit[kFilterDigits];
        for (int k = 0; k < kFilterDigits; ++k) {
            fraction = _mm512_mul_pd(fraction, _mm512_set1_pd(58.0));
            const __m512d whole =
                _mm512_roundscale_pd(fraction, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
            digit[k] = _mm512_cvttpd_epi64(whole);
            fraction = _mm512_sub_pd(fraction, whole);
        }
        __mmask8 any = 0;
        const V one = _mm512_set1_epi64(1);
        for (int p = posMin; p <= posMax; ++p) {
            __mmask8 all = 0xFF;
            for (int k = 0; k < letterCount && p + k < kFilterDigits; ++k)
                all &= _mm512_test_epi64_mask(
                    _mm512_srlv_epi64(_mm512_set1_epi64((long long)letters[k]), digit[p + k]), one);
            any |= all;
        }
        return any;
    };
    const __mmask8 lowLanes = half(_mm512_castsi512_si256(b0), _mm512_castsi512_si256(b1));
    const __mmask8 highLanes =
        half(_mm512_extracti64x4_epi64(b0, 1), _mm512_extracti64x4_epi64(b1, 1));
    return (std::uint16_t)((lowLanes | (std::uint16_t(highLanes) << 8)) & notZero);
}

#else // no AVX-512 in this build

bool supported() {
    return false;
}
BatchHasher::BatchHasher(const std::uint8_t *) : first_{}, last_(0) {}
void BatchHasher::hash(const std::uint64_t[4][kLanes], const std::uint64_t[4][kLanes],
                       std::uint32_t[4][kLanes]) const {}
std::uint16_t BatchHasher::candidates(const std::uint32_t[4][kLanes], int, int,
                                      const std::uint64_t *, int) const {
    return 0;
}
#endif
} // namespace vanity::avx512
