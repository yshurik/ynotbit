#include <bmc/clock.h>
#include <bmc/error.h>
#include <bmc/test.h>
#include <chrono>
#include <string>

TEST(harness_checks_values_and_exceptions) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(std::string("a") + "b", "ab");
    CHECK_THROWS(bmc::require(false, "x"));
}

TEST(error_carries_a_code_and_its_text) {
    try {
        bmc::require(false, "Invalid thing", bmc::Code::State);
        CHECK(false);
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::State);
        CHECK_EQ(std::string(e.what()), "Invalid thing");
    }
    // An Error is a std::runtime_error: the Qt layer catches std::exception and reads what().
    try {
        throw bmc::Error(bmc::Code::Io, "x");
    } catch (const std::runtime_error &) {
        return;
    }
    CHECK(false);
}

TEST(clock_follows_the_system_and_can_be_replaced) {
    const auto before = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto now = bmc::now();
    CHECK(now >= before && now <= before + 2);
    bmc::setClockForTesting([] { return int64_t(1700000000); });
    CHECK_EQ(bmc::now(), 1700000000);
    bmc::setClockForTesting(nullptr);
    CHECK(bmc::now() >= before);
}
