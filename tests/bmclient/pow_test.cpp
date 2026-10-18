#include <algorithm>
#include <atomic>
#include <bmc/clock.h>
#include <bmc/pow.h>
#include <bmc/test.h>
#include <bmc/wire.h>
#include <chrono>
#include <memory>
#include <thread>

namespace {
bool waitDone(bmc::ProofOfWork &pow, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pow.done() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return pow.done();
}
// An object to work on: an acknowledgment, 54 bytes, expiring in an hour.
bmc::Bytes work() {
    return bmc::Wire::acknowledgment(bmc::Bytes(32, 'p'), bmc::now() + 3600);
}
bmc::Bytes solve(const bmc::Bytes &object) {
    bmc::ProofOfWork pow;
    pow.start(object);
    CHECK(waitDone(pow, 120000));
    return pow.take();
}
// A solver that answers at once with a nonce that is wrong.
class LyingSolver : public bmc::PowSolver {
  public:
    std::atomic<int> searched{0}, disabled{0};
    std::optional<uint64_t> search(const unsigned char *, uint64_t, uint64_t first,
                                   const std::atomic_bool &) override {
        ++searched;
        return first;
    }
    void disable(const std::string &) override {
        ++disabled;
    }
};
} // namespace

TEST(proof_of_work_finds_a_valid_nonce_and_changes_only_the_nonce) {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned expected = std::max(1u, cores - 1);
    CHECK_EQ(bmc::ProofOfWork::workerCount(), expected); // one core stays free
    const auto object = work();
    bmc::ProofOfWork pow;
    pow.start(object);
    CHECK(waitDone(pow, 120000));
    const auto solved = pow.take();
    CHECK(bmc::ProofOfWork::valid(solved, bmc::now()));
    CHECK_EQ(solved.size(), object.size());
    CHECK(
        std::equal(solved.begin() + 8, solved.end(), object.begin() + 8)); // only the nonce changes
    CHECK(!pow.done() && pow.take().empty());                              // a result is taken once
}

TEST(proof_of_work_stops_promptly_and_the_same_instance_solves_again) {
    const auto object = work();
    bmc::ProofOfWork pow;
    pow.start(object, 1000000, 1000000); // a requirement nobody meets soon
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto began = std::chrono::steady_clock::now();
    pow.stop();
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(1)); // every worker ends
    CHECK(!pow.done() && pow.take().empty());
    pow.start(object);
    CHECK(waitDone(pow, 120000));
    CHECK(bmc::ProofOfWork::valid(pow.take(), bmc::now()));
    {
        // Destroying a running search must not hang or crash.
        bmc::ProofOfWork running;
        running.start(object, 1000000, 1000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

TEST(proof_of_work_valid_checks_the_expiry_window_the_nonce_and_the_requirement) {
    const int64_t at = bmc::now();
    const auto solved = solve(work());
    CHECK(bmc::ProofOfWork::valid(solved, at));
    CHECK(!bmc::ProofOfWork::valid(solved, at + 3700));                      // expired
    CHECK(!bmc::ProofOfWork::valid(solved, at - 28 * 86400 - 10800 - 3700)); // too far ahead
    CHECK(!bmc::ProofOfWork::valid(bmc::Bytes(100, 0), at));                 // no object
    CHECK(!bmc::ProofOfWork::valid(bmc::Bytes(10, 0), at));
    auto flipped = solved;
    flipped[0] ^= 0x80;
    CHECK(!bmc::ProofOfWork::valid(flipped, at));               // another nonce
    CHECK(!bmc::ProofOfWork::valid(solved, at, 2000000, 1000)); // a requirement over the limit
    CHECK(!bmc::ProofOfWork::valid(solved, at, 1000, 2000000));
}

TEST(proof_of_work_trial_value_is_a_double_sha512_of_the_nonce_and_the_initial_hash) {
    unsigned char initial[64];
    for (int i = 0; i < 64; ++i)
        initial[i] = uint8_t(i * 37 + 11);
    const auto a = bmc::ProofOfWork::trialValue(12345, initial),
               b = bmc::ProofOfWork::trialValue(12346, initial);
    CHECK(a != b);
    CHECK_EQ(a, bmc::ProofOfWork::trialValue(12345, initial));
}

TEST(proof_of_work_ignores_a_solver_that_lies_and_tells_it_to_disable) {
    auto liar = std::make_shared<LyingSolver>();
    bmc::ProofOfWork::setSolver(liar);
    bmc::ProofOfWork pow;
    pow.start(work());
    CHECK(waitDone(pow, 120000));
    CHECK(
        bmc::ProofOfWork::valid(pow.take(), bmc::now())); // the CPU workers' nonce is the one used
    pow.stop();
    CHECK(liar->searched >= 1);
    CHECK(liar->disabled >= 1);
    // Switched off, the solver is not asked.
    liar->searched = 0;
    bmc::ProofOfWork::setSolverEnabled(false);
    CHECK(!bmc::ProofOfWork::solverEnabled());
    pow.start(work());
    CHECK(waitDone(pow, 120000));
    CHECK(bmc::ProofOfWork::valid(pow.take(), bmc::now()));
    pow.stop();
    CHECK_EQ(liar->searched.load(), 0);
    bmc::ProofOfWork::setSolverEnabled(true);
    bmc::ProofOfWork::setSolver(nullptr);
}

TEST(proof_of_work_refuses_an_object_that_is_not_one) {
    bmc::ProofOfWork pow;
    CHECK_THROWS(pow.start(bmc::Bytes()));       // nothing at all
    CHECK_THROWS(pow.start(bmc::unhex("0000"))); // shorter than a header
    CHECK(!pow.done());                          // and no worker was left running for it
}
