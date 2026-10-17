#include <atomic>
#include <bmc/clock.h>
#include <chrono>

namespace bmc {
namespace {
std::atomic<int64_t (*)()> replacement{nullptr};
} // namespace

int64_t now() {
    if (auto clock = replacement.load())
        return clock();
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t nowMillis() {
    // Qt's QDateTime::currentMSecsSinceEpoch() rounds to the nearest millisecond, so a chat's
    // sequence number is the same number the Qt app would have written. Truncating would put
    // the core a millisecond behind Qt's clock about half the time.
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return (micros + 500) / 1000;
}

void setClockForTesting(int64_t (*clock)()) {
    replacement = clock;
}
} // namespace bmc
