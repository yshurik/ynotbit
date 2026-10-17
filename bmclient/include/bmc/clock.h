#pragma once
#include <cstdint>

namespace bmc {
// Seconds since the epoch.
int64_t now();
// Milliseconds since the epoch: a chat's seqno, which keeps growing without a counter.
int64_t nowMillis();
// For tests: a clock of their own. nullptr restores the real one.
void setClockForTesting(int64_t (*clock)());
} // namespace bmc
