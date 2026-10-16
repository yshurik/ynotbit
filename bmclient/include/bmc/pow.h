// Proof of work for a Bitmessage object: CPU workers, and room for a solver of another kind (a
// GPU).
#pragma once
#include <atomic>
#include <bmc/bytes.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bmc {
// A search that runs beside the CPU workers; the Qt app's GPU is one. Whatever it returns is
// checked again on the CPU before it is used.
class PowSolver {
  public:
    virtual ~PowSolver() = default;
    // Searches nonces from `first` upward for one whose trial value is at most `target`, until
    // found or `cancel` is set. `initial` is the 64-byte hash of the object without its nonce.
    virtual std::optional<uint64_t> search(const unsigned char *initial, uint64_t target,
                                           uint64_t first, const std::atomic_bool &cancel) = 0;
    // After a wrong answer the solver is not trusted again in this run.
    virtual void disable(const std::string &why) = 0;
};

class ProofOfWork {
    std::atomic_bool cancel_{false}, done_{false};
    std::vector<std::thread> workers_;
    std::mutex resultMutex_;
    Bytes result_;
    void join();

  public:
    ~ProofOfWork() {
        stop();
    }
    // Searches on workerCount() threads, and on the solver if one is set and enabled; done()
    // turns true when one finds a nonce. Throws if the object is none or the requirement is out of
    // range.
    void start(Bytes object, uint64_t trials = 1000, uint64_t extra = 1000);
    void stop();
    bool done() const {
        return done_;
    }
    // The solved object, once; empty if there is none.
    Bytes take();
    // Every core but one, so the window stays responsive while a letter is prepared.
    static unsigned workerCount();
    // The solver searches alongside the CPU workers (null: CPU only).
    static void setSolver(std::shared_ptr<PowSolver> solver);
    static void setSolverEnabled(bool on);
    static bool solverEnabled();
    // The value the network compares with the target for this nonce.
    static uint64_t trialValue(uint64_t nonce, const unsigned char initial[64]);
    // Does the object carry enough work at time `at`, and does it expire within the window the
    // network accepts?
    static bool valid(ByteView object, int64_t at, uint64_t trials = 1000, uint64_t extra = 1000);
};
} // namespace bmc
