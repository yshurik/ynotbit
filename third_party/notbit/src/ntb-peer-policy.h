#ifndef NTB_PEER_POLICY_H
#define NTB_PEER_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* Shared pure policy; timestamps for runtime deadlines are monotonic microseconds. */
static inline bool ntb_peer_setup_expired(uint64_t now, uint64_t started, bool proxy)
{
        return now >= started && now - started >= (proxy ? 60 : 20) * UINT64_C(1000000);
}
static inline unsigned ntb_peer_backoff(unsigned failures, unsigned jitter)
{
        unsigned seconds = failures > 6 ? 3600 : 60u << (failures ? failures - 1 : 0);
        seconds += jitter % 16;
        return seconds > 3600 ? 3600 : seconds;
}
static inline bool ntb_peer_retained(int64_t now, int64_t advertised, int64_t success)
{
        return (advertised > 0 && advertised <= now && now - advertised < 86400) ||
               (success > 0 && success <= now && now - success < 28 * 86400);
}
static inline bool ntb_peer_should_ping(uint64_t now, uint64_t activity, uint64_t last_ping)
{
        return now >= activity && now - activity >= UINT64_C(300000000) &&
               now >= last_ping && now - last_ping >= UINT64_C(300000000);
}
#endif
