#pragma once
#include <cstddef>

namespace bmc {
// Memory for keys: from libsodium's guarded allocator, zeroed at birth, wiped and freed at death.
class Secret {
    unsigned char *p_ = nullptr;
    size_t n_ = 0;

  public:
    explicit Secret(size_t n = 0);
    ~Secret();
    Secret(Secret &&other) noexcept;
    Secret &operator=(Secret &&other) noexcept;
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;
    unsigned char *data() {
        return p_;
    }
    const unsigned char *data() const {
        return p_;
    }
    size_t size() const {
        return n_;
    }
    void clear();
};
} // namespace bmc
