#pragma once
// One way of running the proof-of-work kernel on a GPU: OpenCL, or Metal on
// macOS. GpuSolver drives a backend one batch at a time.
#include <QString>
#include <QtGlobal>
#include <memory>
#include <optional>
namespace bm::gpu {
// One batch's outcome: a nonce whose trial value is at most the target, if
// the batch met one, or what went wrong.
struct Batch {
    std::optional<quint64> nonce;
    QString error; // empty unless the GPU failed
};
class Backend {
  public:
    virtual ~Backend() = default;
    virtual QString api() const = 0;    // "Metal" or "OpenCL"
    virtual QString device() const = 0; // the GPU's name, e.g. "Apple M1"
    // Work items in one batch.
    virtual quint64 width() const = 0;
    // The 64-byte initial hash every trial uses, as eight big-endian words;
    // also forgets a nonce found before. Empty, or what went wrong.
    virtual QString begin(const quint64 initial[8]) = 0;
    // Work item i tries the perItem nonces from base + i * perItem.
    virtual Batch run(quint64 base, quint32 perItem, quint64 target) = 0;
};
// Null, with *problem saying why, when this machine has no such GPU.
std::unique_ptr<Backend> openOpenCL(QString *problem);
#ifdef Q_OS_MACOS
std::unique_ptr<Backend> openMetal(QString *problem);
#endif

// Bitmessage's trial value, as GPU code shared by the backends: the first 8
// bytes, big-endian, of SHA512(SHA512(nonce || initial)). Both messages fit
// one SHA-512 block. A backend defines ROR(x, n), CONSTANT (its address space
// for constants) and THREAD (for a work item's own memory) before it.
inline const char *kTrialSource = R"GPU(
CONSTANT ulong K[80] = {
0x428a2f98d728ae22UL, 0x7137449123ef65cdUL, 0xb5c0fbcfec4d3b2fUL, 0xe9b5dba58189dbbcUL,
0x3956c25bf348b538UL, 0x59f111f1b605d019UL, 0x923f82a4af194f9bUL, 0xab1c5ed5da6d8118UL,
0xd807aa98a3030242UL, 0x12835b0145706fbeUL, 0x243185be4ee4b28cUL, 0x550c7dc3d5ffb4e2UL,
0x72be5d74f27b896fUL, 0x80deb1fe3b1696b1UL, 0x9bdc06a725c71235UL, 0xc19bf174cf692694UL,
0xe49b69c19ef14ad2UL, 0xefbe4786384f25e3UL, 0x0fc19dc68b8cd5b5UL, 0x240ca1cc77ac9c65UL,
0x2de92c6f592b0275UL, 0x4a7484aa6ea6e483UL, 0x5cb0a9dcbd41fbd4UL, 0x76f988da831153b5UL,
0x983e5152ee66dfabUL, 0xa831c66d2db43210UL, 0xb00327c898fb213fUL, 0xbf597fc7beef0ee4UL,
0xc6e00bf33da88fc2UL, 0xd5a79147930aa725UL, 0x06ca6351e003826fUL, 0x142929670a0e6e70UL,
0x27b70a8546d22ffcUL, 0x2e1b21385c26c926UL, 0x4d2c6dfc5ac42aedUL, 0x53380d139d95b3dfUL,
0x650a73548baf63deUL, 0x766a0abb3c77b2a8UL, 0x81c2c92e47edaee6UL, 0x92722c851482353bUL,
0xa2bfe8a14cf10364UL, 0xa81a664bbc423001UL, 0xc24b8b70d0f89791UL, 0xc76c51a30654be30UL,
0xd192e819d6ef5218UL, 0xd69906245565a910UL, 0xf40e35855771202aUL, 0x106aa07032bbd1b8UL,
0x19a4c116b8d2d0c8UL, 0x1e376c085141ab53UL, 0x2748774cdf8eeb99UL, 0x34b0bcb5e19b48a8UL,
0x391c0cb3c5c95a63UL, 0x4ed8aa4ae3418acbUL, 0x5b9cca4f7763e373UL, 0x682e6ff3d6b2b8a3UL,
0x748f82ee5defb2fcUL, 0x78a5636f43172f60UL, 0x84c87814a1f0ab72UL, 0x8cc702081a6439ecUL,
0x90befffa23631e28UL, 0xa4506cebde82bde9UL, 0xbef9a3f7b2c67915UL, 0xc67178f2e372532bUL,
0xca273eceea26619cUL, 0xd186b8c721c0c207UL, 0xeada7dd6cde0eb1eUL, 0xf57d4f7fee6ed178UL,
0x06f067aa72176fbaUL, 0x0a637dc5a2c898a6UL, 0x113f9804bef90daeUL, 0x1b710b35131c471bUL,
0x28db77f523047d84UL, 0x32caab7b40c72493UL, 0x3c9ebe0a15c9bebcUL, 0x431d67c49c100d4cUL,
0x4cc5d4becb3e42b6UL, 0x597f299cfc657e2aUL, 0x5fcb6fab3ad6faecUL, 0x6c44198c4a475817UL};
CONSTANT ulong H0[8] = {
0x6a09e667f3bcc908UL, 0xbb67ae8584caa73bUL, 0x3c6ef372fe94f82bUL, 0xa54ff53a5f1d36f1UL,
0x510e527fade682d1UL, 0x9b05688c2b3e6c1fUL, 0x1f83d9abfb41bd6bUL, 0x5be0cd19137e2179UL};
static void compress(THREAD ulong *h, THREAD ulong *w) {
    for (int i = 16; i < 80; i++) {
        ulong s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        ulong s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    ulong a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        ulong t1 = hh + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        ulong t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static ulong trial_value(ulong nonce, CONSTANT ulong *initial) {
    ulong w[80], h[8];
    w[0] = nonce;
    for (int i = 0; i < 8; i++) w[1 + i] = initial[i];
    w[9] = 0x8000000000000000UL;
    for (int i = 10; i < 15; i++) w[i] = 0;
    w[15] = 72 * 8;
    for (int i = 0; i < 8; i++) h[i] = H0[i];
    compress(h, w);
    for (int i = 0; i < 8; i++) w[i] = h[i];
    w[8] = 0x8000000000000000UL;
    for (int i = 9; i < 15; i++) w[i] = 0;
    w[15] = 64 * 8;
    for (int i = 0; i < 8; i++) h[i] = H0[i];
    compress(h, w);
    return h[0];
}
)GPU";
} // namespace bm::gpu
