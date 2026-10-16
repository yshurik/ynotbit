// What the old protocol.cpp, protocol_wire.cpp and pow.cpp took from src/storage.h, in the
// namespace legacy. Written by hand next to the snapshot made by scripts/legacy-snapshot.sh.
#pragma once
#include <QByteArray>
#include <QString>
#include <new>
#include <sodium.h>
#include <stdexcept>
#include <utility>

namespace legacy {
constexpr int kMaxObjectBytes = 262144;
constexpr int kMaxLetterText = 261000;
inline int letterTextBytes(const QString &subject, const QString &body) {
    return int(("Subject:" + subject + "\nBody:" + body).toUtf8().size());
}
class Secret {
    unsigned char *p_ = nullptr;
    size_t n_ = 0;

  public:
    explicit Secret(size_t n = 0) : n_(n) {
        if (sodium_init() < 0)
            throw std::runtime_error("Cannot initialize cryptography");
        if (n) {
            p_ = static_cast<unsigned char *>(sodium_malloc(n));
            if (!p_)
                throw std::bad_alloc();
            sodium_memzero(p_, n);
        }
    }
    ~Secret() {
        clear();
    }
    Secret(Secret &&o) noexcept : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}
    Secret &operator=(Secret &&o) noexcept {
        if (this != &o) {
            clear();
            p_ = std::exchange(o.p_, nullptr);
            n_ = std::exchange(o.n_, 0);
        }
        return *this;
    }
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
    void clear() {
        if (p_)
            sodium_free(p_);
        p_ = nullptr;
        n_ = 0;
    }
};
struct Identity {
    QString label, address;
    bool chan = false;
    bool isDefault = false;
    Secret keys{64};
    Identity() = default;
    Identity(Identity &&) = default;
    Identity &operator=(Identity &&) = default;
};
struct Message {
    QString hash, from, to, subject, body, folder;
    qint64 received = 0;
};
} // namespace legacy
