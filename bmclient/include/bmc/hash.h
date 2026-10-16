#pragma once
#include <bmc/bytes.h>
#include <memory>

namespace bmc {
enum class Digest { Sha1, Sha256, Sha512 };

Bytes hash(Digest digest, ByteView data);

// The same digest in pieces.
class Hasher {
  public:
    explicit Hasher(Digest digest);
    ~Hasher();
    Hasher(const Hasher &) = delete;
    Hasher &operator=(const Hasher &) = delete;
    void add(ByteView data);
    // Ends the hash; the Hasher is not used afterwards.
    Bytes result();

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace bmc
