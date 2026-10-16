#include <bmc/random.h>
#include <bmc/secret.h>
#include <new>
#include <sodium.h>
#include <utility>

namespace bmc {
Secret::Secret(size_t n) : n_(n) {
    initCrypto();
    if (n) {
        p_ = static_cast<unsigned char *>(sodium_malloc(n));
        if (!p_)
            throw std::bad_alloc();
        sodium_memzero(p_, n);
    }
}

Secret::~Secret() {
    clear();
}

void Secret::clear() {
    if (p_)
        sodium_free(p_);
    p_ = nullptr;
    n_ = 0;
}

Secret::Secret(Secret &&o) noexcept
    : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}

Secret &Secret::operator=(Secret &&o) noexcept {
    if (this != &o) {
        clear();
        p_ = std::exchange(o.p_, nullptr);
        n_ = std::exchange(o.n_, 0);
    }
    return *this;
}
} // namespace bmc
