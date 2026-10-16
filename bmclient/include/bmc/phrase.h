// Keys for a new identity from a secret phrase, as PyBitmessage derives a deterministic address:
// signing key = SHA-512(phrase + varint(2 pair))[:32], encryption key = SHA-512(phrase +
// varint(2 pair + 1))[:32]; a version 4 address in stream 1, not a chan.
#pragma once
#include <bmc/identity.h>
#include <memory>
#include <optional>
#include <string_view>

namespace bmc::phrase {
// A libsecp256k1 context of one's own: making public keys needs one.
class Context {
  public:
    Context();
    ~Context();
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    // The zero bytes a key pair's RIPE hash starts with; -1 for a key out of range.
    int pairZeroBytes(const unsigned char sign[32], const unsigned char enc[32]) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
// One of a phrase's private keys: SHA-512(phrase + varint(nonce))[:32].
void key(std::string_view phrase, uint64_t nonce, unsigned char out[32]);
// An identity (no label) from its two private keys.
Identity fromKeys(const unsigned char sign[32], const unsigned char enc[32]);
Identity identity(std::string_view phrase, std::string_view label, uint64_t pair);
// The first of a phrase's key pairs whose RIPE hash starts with at least zeroBytes zero bytes,
// looking at no more than limit pairs. 1 gives the phrase's standard address, PyBitmessage's first;
// 2 its "shorter" one.
std::optional<uint64_t> firstPair(std::string_view phrase, int zeroBytes, uint64_t limit = 1000000);
// How many zero bytes an address's RIPE hash starts with; 0 if it is no address.
int zeroBytes(std::string_view address);
} // namespace bmc::phrase
