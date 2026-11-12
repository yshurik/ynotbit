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

TEST(the_expected_work_follows_the_size_the_life_and_the_requirements_of_the_object) {
    using bmc::ProofOfWork;
    // The network's own formula: trials x (length + length x life / 65536), the length being the
    // size and the extra bytes.
    CHECK(ProofOfWork::expectedTrials(0, 0, 1000, 1000) == 1e6);
    CHECK(ProofOfWork::expectedTrials(1000, 0, 1000, 1000) == 2e6);
    CHECK(ProofOfWork::expectedTrials(1000, 65536, 1000, 1000) ==
          4e6); // a life of 65536 s doubles it
    CHECK(ProofOfWork::expectedTrials(1000, 0, 3000, 1000) == 6e6);
    CHECK(ProofOfWork::expectedTrials(1000, 0, 1000, 3000) == 4e6);
    // A life that is over counts as none, and a requirement below the minimum is the minimum.
    CHECK(ProofOfWork::expectedTrials(1000, -60, 1000, 1000) == 2e6);
    CHECK(ProofOfWork::expectedTrials(1000, 0, 1, 1) == 2e6);
    // The defaults are the minimum of the network.
    CHECK(ProofOfWork::expectedTrials(1000, 0) == 2e6);
    // A page of 70 KB that lives a day against a short reply: the gap the order of mining is about.
    CHECK(ProofOfWork::expectedTrials(70000, 86400) >
          30 * ProofOfWork::expectedTrials(1000, 86400));
}

TEST(workers_are_every_core_but_one_or_the_cpu_quota_a_container_has) {
    using bmc::ProofOfWork;
    CHECK_EQ(ProofOfWork::workerCountFor(56, 0), 55u); // no limit: one core stays free
    CHECK_EQ(ProofOfWork::workerCountFor(4, 0), 3u);
    CHECK_EQ(ProofOfWork::workerCountFor(1, 0), 1u);
    CHECK_EQ(ProofOfWork::workerCountFor(2, 0), 1u);
    // A container allowed 2 cores of a big host (56 here): a thread per core of the host would only
    // take its turn in bursts of the quota (and keep the cores slow), so it is the quota, rounded
    // down, and all of it: a bot has nothing else to keep responsive.
    CHECK_EQ(ProofOfWork::workerCountFor(56, 2.0), 2u);
    CHECK_EQ(ProofOfWork::workerCountFor(56, 2.5), 2u);
    CHECK_EQ(ProofOfWork::workerCountFor(56, 8.0), 8u);
    CHECK_EQ(ProofOfWork::workerCountFor(56, 0.5), 1u); // never none
    // A quota that is the whole machine, or more, is no limit.
    CHECK_EQ(ProofOfWork::workerCountFor(4, 4.0), 3u);
    CHECK_EQ(ProofOfWork::workerCountFor(4, 8.0), 3u);
}

TEST(a_cgroups_cpu_quota_is_read_in_cores) {
    using bmc::ProofOfWork;
    CHECK_EQ(ProofOfWork::parseCpuMax("200000 100000\n"), 2.0);
    CHECK_EQ(ProofOfWork::parseCpuMax("50000 100000"), 0.5);
    CHECK_EQ(ProofOfWork::parseCpuMax("800000 100000"), 8.0);
    CHECK_EQ(ProofOfWork::parseCpuMax("max 100000\n"), 0.0); // no limit
    CHECK_EQ(ProofOfWork::parseCpuMax(""), 0.0);
    CHECK_EQ(ProofOfWork::parseCpuMax("garbage"), 0.0);
    CHECK_EQ(ProofOfWork::parseCpuMax("0 100000"), 0.0);
    CHECK_EQ(ProofOfWork::parseCpuMax("100000 0"), 0.0);
    CHECK_EQ(ProofOfWork::parseCpuMax("-1 100000"), 0.0);
}

TEST(proof_of_work_finds_a_valid_nonce_and_changes_only_the_nonce) {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    CHECK_EQ(bmc::ProofOfWork::workerCount(),
             bmc::ProofOfWork::workerCountFor(cores, bmc::ProofOfWork::cpuLimit()));
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
