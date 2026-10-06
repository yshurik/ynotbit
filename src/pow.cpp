#include "pow.h"
#include "protocol_wire.h"
#include <QDateTime>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <openssl/sha.h>
#include <stdexcept>
namespace bm {
static quint64 target(qsizetype size, qint64 ttl, quint64 trials, quint64 extra) {
    if (size < 22 || size > 262144 || trials > 1000000 || extra > 1000000)
        throw std::runtime_error("Unsupported proof-of-work requirement");
    trials = std::max<quint64>(trials, 1000);
    extra = std::max<quint64>(extra, 1000);
    const quint64 length = size + extra;
    const quint64 divisor = trials * (length + (length * std::max<qint64>(ttl, 0)) / 65536);
    // floor(2^64 / divisor), without unsigned overflow.
    return std::numeric_limits<quint64>::max() / divisor +
           (std::numeric_limits<quint64>::max() % divisor == divisor - 1);
}
static quint64 value(const unsigned char *nonce, const unsigned char *initial) {
    unsigned char first[64], second[64];
    SHA512_CTX ctx;
    SHA512_Init(&ctx);
    SHA512_Update(&ctx, nonce, 8);
    SHA512_Update(&ctx, initial, 64);
    SHA512_Final(first, &ctx);
    // Not the one-shot SHA512(): in OpenSSL 3 it looks the digest up under a
    // global lock, which serialises the workers.
    SHA512_Init(&ctx);
    SHA512_Update(&ctx, first, 64);
    SHA512_Final(second, &ctx);
    return qFromBigEndian<quint64>(second);
}
bool ProofOfWork::valid(const QByteArray &object, qint64 now, quint64 trials, quint64 extra) {
    auto h = Wire::header(object);
    if (!h || h->expires < now || h->expires > now + 28 * 86400 + 10800)
        return false;
    try {
        unsigned char initial[64];
        SHA512(reinterpret_cast<const unsigned char *>(object.constData() + 8), object.size() - 8,
               initial);
        return value(reinterpret_cast<const unsigned char *>(object.constData()), initial) <=
               target(object.size(), h->expires - now, trials, extra);
    } catch (...) {
        return false;
    }
}
void ProofOfWork::start(QByteArray object, quint64 trials, quint64 extra) {
    stop();
    auto h = Wire::header(object);
    if (!h)
        throw std::runtime_error("Invalid proof-of-work object");
    auto threshold =
        target(object.size(), h->expires - QDateTime::currentSecsSinceEpoch(), trials, extra);
    cancel_ = false;
    done_ = false;
    unsigned char initial[64];
    SHA512(reinterpret_cast<const unsigned char *>(object.constData() + 8), object.size() - 8,
           initial);
    // A resumed job must not repeatedly search the same nonce prefix after each lock.
    quint64 firstNonce;
    randombytes_buf(&firstNonce, sizeof firstNonce);
    const unsigned count = workerCount();
    for (unsigned i = 0; i < count; ++i) {
        // Worker i tries firstNonce + i, + i + count, ...: no two try the same nonce.
        workers_.emplace_back([this, object, initial = std::to_array(initial), threshold,
                               first = firstNonce + i, count]() mutable {
            unsigned char nonceBytes[8];
            for (quint64 nonce = first; !cancel_; nonce += count) {
                qToBigEndian(nonce, nonceBytes);
                if (value(nonceBytes, initial.data()) > threshold)
                    continue;
                std::lock_guard lock(resultMutex_);
                if (done_ || cancel_)
                    return;
                std::copy(nonceBytes, nonceBytes + 8, object.data());
                result_ = std::move(object);
                done_ = true;
                cancel_ = true; // the others stop at their next nonce
                return;
            }
        });
    }
}
unsigned ProofOfWork::workerCount() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores > 1 ? cores - 1 : 1;
}
void ProofOfWork::join() {
    for (auto &worker : workers_)
        if (worker.joinable())
            worker.join();
    workers_.clear();
}
void ProofOfWork::stop() {
    cancel_ = true;
    join();
    result_.fill(0);
    result_.clear();
    done_ = false;
}
QByteArray ProofOfWork::take() {
    if (!done_)
        return {};
    join();
    auto result = std::move(result_);
    done_ = false;
    return result;
}
} // namespace bm
