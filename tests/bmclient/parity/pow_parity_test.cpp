#include "legacy/oracle.h"
#include <array>
#include <bmc/clock.h>
#include <bmc/pow.h>
#include <bmc/test.h>
#include <bmc/wire.h>
#include <chrono>
#include <random>
#include <thread>

TEST(trial_value_matches_the_old_code) {
    std::mt19937_64 rng(0x5eed000b);
    for (int round = 0; round < 5000; ++round) {
        std::array<uint8_t, 64> initial;
        for (auto &b : initial)
            b = uint8_t(rng());
        const uint64_t nonce = rng();
        CHECK_EQ(bmc::ProofOfWork::trialValue(nonce, initial.data()),
                 oracle::trialValue(nonce, initial));
    }
}

TEST(valid_matches_the_old_code_on_solved_and_damaged_objects) {
    std::mt19937_64 rng(0x5eed000c);
    const int64_t at = bmc::now();
    // One piece of work, about a second, judged by both under many requirements.
    bmc::ProofOfWork pow;
    pow.start(bmc::Wire::acknowledgment(bmc::Bytes(32, 'q'), at + 600));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!pow.done() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto solved = pow.take();
    CHECK(!solved.empty());
    for (int64_t when : {at - 3000000, at - 100, at, at + 599, at + 600, at + 601, at + 100000})
        for (uint64_t trials : {0ull, 1ull, 1000ull, 1001ull, 5000ull, 1000000ull, 1000001ull})
            for (uint64_t extra : {0ull, 1000ull, 4000ull, 1000000ull, 1000001ull})
                CHECK_EQ(bmc::ProofOfWork::valid(solved, when, trials, extra),
                         oracle::powValid(solved, when, trials, extra));
    for (int round = 0; round < 3000; ++round) {
        auto damaged = solved;
        damaged[rng() % damaged.size()] ^= uint8_t(1 << (rng() % 8));
        if (rng() % 6 == 0)
            damaged.resize(rng() % (damaged.size() + 1));
        CHECK_EQ(bmc::ProofOfWork::valid(damaged, at, 1000, 1000),
                 oracle::powValid(damaged, at, 1000, 1000));
    }
}
