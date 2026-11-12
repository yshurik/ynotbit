#include <algorithm>
#include <array>
#include <bmc/clock.h>
#include <bmc/pow.h>
#include <bmc/random.h>
#include <bmc/wire.h>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <openssl/sha.h>
#include <stdexcept>

namespace bmc {
namespace {
std::atomic_bool solverOn{true};
std::mutex solverLock;
std::shared_ptr<PowSolver> &solverSlot() {
    static std::shared_ptr<PowSolver> solver;
    return solver;
}
std::shared_ptr<PowSolver> currentSolver() {
    std::lock_guard guard(solverLock);
    return solverSlot();
}

uint64_t target(size_t size, int64_t ttl, uint64_t trials, uint64_t extra) {
    if (size < 22 || size > 262144 || trials > 1000000 || extra > 1000000)
        throw std::runtime_error("Unsupported proof-of-work requirement");
    trials = std::max<uint64_t>(trials, 1000);
    extra = std::max<uint64_t>(extra, 1000);
    const uint64_t length = size + extra;
    const uint64_t divisor =
        trials * (length + (length * uint64_t(std::max<int64_t>(ttl, 0))) / 65536);
    // floor(2^64 / divisor), without unsigned overflow.
    return std::numeric_limits<uint64_t>::max() / divisor +
           (std::numeric_limits<uint64_t>::max() % divisor == divisor - 1);
}
void storeBE64(uint64_t v, unsigned char out[8]) {
    for (int i = 7; i >= 0; --i) {
        out[i] = uint8_t(v);
        v >>= 8;
    }
}
uint64_t loadBE64(const unsigned char in[8]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | in[i];
    return v;
}
uint64_t value(const unsigned char *nonce, const unsigned char *initial) {
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
    return loadBE64(second);
}
} // namespace

bool ProofOfWork::valid(ByteView object, int64_t at, uint64_t trials, uint64_t extra) {
    auto h = Wire::header(object);
    if (!h || h->expires < at || h->expires > at + 28 * 86400 + 10800)
        return false;
    try {
        unsigned char initial[64];
        SHA512(object.data() + 8, object.size() - 8, initial);
        return value(object.data(), initial) <=
               target(object.size(), h->expires - at, trials, extra);
    } catch (...) {
        return false;
    }
}

void ProofOfWork::start(Bytes object, uint64_t trials, uint64_t extra) {
    stop();
    auto h = Wire::header(object);
    if (!h)
        throw std::runtime_error("Invalid proof-of-work object");
    const auto threshold = target(object.size(), h->expires - now(), trials, extra);
    cancel_ = false;
    done_ = false;
    unsigned char initial[64];
    SHA512(object.data() + 8, object.size() - 8, initial);
    // A resumed job must not repeatedly search the same nonce prefix after each lock.
    uint64_t firstNonce;
    randomBytes(&firstNonce, sizeof firstNonce);
    // The first worker to find a nonce keeps it; the others stop at their next one.
    const auto win = [this](Bytes &object, const unsigned char *nonceBytes) {
        std::lock_guard lock(resultMutex_);
        if (done_ || cancel_)
            return;
        std::copy(nonceBytes, nonceBytes + 8, object.data());
        result_ = std::move(object);
        done_ = true;
        cancel_ = true;
    };
    const unsigned count = workerCount();
    for (unsigned i = 0; i < count; ++i) {
        // Worker i tries firstNonce + i, + i + count, ...: no two try the same nonce.
        workers_.emplace_back([this, win, object, initial = std::to_array(initial), threshold,
                               first = firstNonce + i, count]() mutable {
            unsigned char nonceBytes[8];
            for (uint64_t nonce = first; !cancel_; nonce += count) {
                storeBE64(nonce, nonceBytes);
                if (value(nonceBytes, initial.data()) <= threshold)
                    return win(object, nonceBytes);
            }
        });
    }
    const auto solver = solverOn ? currentSolver() : nullptr;
    if (!solver)
        return;
    // The solver searches the other half of the nonce space.
    workers_.emplace_back([this, win, object, initial = std::to_array(initial), threshold,
                           first = firstNonce ^ (uint64_t(1) << 63), solver]() mutable {
        const auto nonce = solver->search(initial.data(), threshold, first, cancel_);
        if (!nonce)
            return;
        unsigned char nonceBytes[8];
        storeBE64(*nonce, nonceBytes);
        // A driver bug must never send a letter the network rejects.
        if (value(nonceBytes, initial.data()) > threshold)
            return solver->disable("the GPU returned a wrong proof of work");
        win(object, nonceBytes);
    });
}

void ProofOfWork::setSolver(std::shared_ptr<PowSolver> solver) {
    std::lock_guard guard(solverLock);
    solverSlot() = std::move(solver);
}
void ProofOfWork::setSolverEnabled(bool on) {
    solverOn = on;
}
bool ProofOfWork::solverEnabled() {
    return solverOn;
}
uint64_t ProofOfWork::trialValue(uint64_t nonce, const unsigned char initial[64]) {
    unsigned char nonceBytes[8];
    storeBE64(nonce, nonceBytes);
    return value(nonceBytes, initial);
}
double ProofOfWork::expectedTrials(size_t size, int64_t ttl, uint64_t trials, uint64_t extra) {
    // target()'s divisor, with the same floors, in a double: 2^64 / it is the threshold.
    const double length = double(size) + double(std::max<uint64_t>(extra, 1000));
    return double(std::max<uint64_t>(trials, 1000)) *
           (length + length * double(std::max<int64_t>(ttl, 0)) / 65536.0);
}
unsigned ProofOfWork::workerCountFor(unsigned cores, double cpuLimit) {
    if (cpuLimit > 0 && cpuLimit < cores)
        return std::max(1u, unsigned(cpuLimit));
    return cores > 1 ? cores - 1 : 1;
}
unsigned ProofOfWork::workerCount() {
    return workerCountFor(std::thread::hardware_concurrency(), cpuLimit());
}
double ProofOfWork::parseCpuMax(std::string_view text) {
    const auto space = text.find(' ');
    if (space == std::string_view::npos)
        return 0;
    const std::string quota(text.substr(0, space));
    std::string period(text.substr(space + 1));
    while (!period.empty() &&
           (period.back() == '\n' || period.back() == '\r' || period.back() == ' '))
        period.pop_back();
    char *end = nullptr;
    const double q = std::strtod(quota.c_str(), &end);
    if (quota.empty() || *end != '\0')
        return 0;
    const double p = std::strtod(period.c_str(), &end);
    if (period.empty() || *end != '\0' || q <= 0 || p <= 0)
        return 0;
    return q / p;
}
double ProofOfWork::cpuLimit() {
    static const auto read = [](const char *path) {
        std::ifstream in(path);
        std::string text, line;
        while (std::getline(in, line))
            text += line + "\n";
        return text;
    };
    // cgroup v2
    if (const auto v2 = read("/sys/fs/cgroup/cpu.max"); !v2.empty())
        return parseCpuMax(v2);
    // cgroup v1
    const auto quota = read("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    const auto period = read("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (quota.empty() || period.empty())
        return 0;
    return parseCpuMax(quota.substr(0, quota.find('\n')) + " " + period);
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
    wipe(result_);
    done_ = false;
}
Bytes ProofOfWork::take() {
    if (!done_)
        return {};
    join();
    auto result = std::move(result_);
    result_.clear();
    done_ = false;
    return result;
}
} // namespace bmc
