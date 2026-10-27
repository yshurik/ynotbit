#pragma once
// The two hashes of a vanity search, sixteen candidates at a time, for processors with AVX-512.
//
// vanity.h hashes the candidates one after the other, with OpenSSL's block functions: about two
// thirds of what a search costs. Here the same SHA-512 and RIPEMD-160 run on the lanes of 512-bit
// registers, SHA-512 on eight 64-bit lanes (twice) and RIPEMD-160 on sixteen 32-bit lanes, and the
// first characters of the addresses are read off the RIPEs in the registers too, so that only the
// few candidates that may have the word reach the scalar code.
//
// The code is in avx512.cpp, which is built with -mavx512f -mavx512bw -mavx512dq -mavx512vl (or
// -march=native on a processor that has them). Anywhere else supported() is false and the rest is
// empty, and a search uses the scalar hashes.
#include <cstdint>

namespace vanity::avx512 {
constexpr int kLanes = 16;
// How many digits of an address the filter reads: it checks a letter of the word only where the
// letter's place is below this, so it can say "may have the word" for a word that is further in.
constexpr int kFilterDigits = 7;

// Whether this build has the code and this processor can run it.
bool supported();

class BatchHasher {
  public:
    // signingPublic is 04, X and Y of the signing key's public key: the first 65 bytes of what is
    // hashed for every candidate.
    explicit BatchHasher(const std::uint8_t signingPublic[65]);

    // The RIPE of sixteen encryption keys, given by the limbs of the x and y of their public
    // keys (xs[k][i] is limb k of key i, limb 0 the lowest). words[w][i] is the w-th 32-bit word,
    // little-endian as RIPEMD-160 leaves them, of key i's RIPE: its first 16 bytes, which is all
    // that finding a word in an address needs.
    void hash(const std::uint64_t xs[4][kLanes], const std::uint64_t ys[4][kLanes],
              std::uint32_t words[4][kLanes]) const;

    // Which of the sixteen RIPEs (bit i is key i) do not start with a zero byte and may be an
    // address with the word at a place from posMin to posMax: the first letters it reads are those
    // below kFilterDigits, from the digits of the address as a number computed in floating point,
    // so that one in a hundred thousand matches might be lost and a lane that passes is only a
    // maybe. letters[k] has bit d set for each base 58 digit d that letter k may be.
    std::uint16_t candidates(const std::uint32_t words[4][kLanes], int posMin, int posMax,
                             const std::uint64_t *letters, int letterCount) const;

  private:
    std::uint64_t first_[8]; // bytes 0 to 63 of the signing key's public key, as big-endian words
    std::uint8_t last_;      // byte 64
};
} // namespace vanity::avx512
