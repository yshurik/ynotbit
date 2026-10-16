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

void setClockForTesting(int64_t (*clock)()) {
    replacement = clock;
}
} // namespace bmc
