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
/* What an address is to this node decides how long a failure keeps it away. */
enum ntb_peer_kind {
        /* Heard of in an addr or from DNS: most of these are dead for good */
        NTB_PEER_DISCOVERED,
        /* Has connected within the last 28 days */
        NTB_PEER_PROVEN,
        /* Named by the operator, with -P */
        NTB_PEER_EXPLICIT
};
static inline bool ntb_peer_proven(int64_t now, int64_t success)
{
        return success > 0 && success <= now && now - success < 28 * 86400;
}
static inline unsigned ntb_peer_backoff_kind(enum ntb_peer_kind kind, unsigned failures,
                                             unsigned jitter)
{
        unsigned seconds;
        switch (kind) {
        case NTB_PEER_EXPLICIT:
                /* A choice, not a guess (a node on this machine, a server the operator
                 * runs): 5, 10, 20 and then every 30 seconds for as long as it takes. */
                seconds = failures > 3 ? 30 : 5u << (failures ? failures - 1 : 0);
                return seconds + jitter % 5;
        case NTB_PEER_PROVEN:
                /* Worth coming back to: it may be our own network that was down. */
                seconds = failures > 3 ? 300 : 60u << (failures ? failures - 1 : 0);
                seconds += jitter % 16;
                return seconds > 300 ? 300 : seconds;
        default:
                return ntb_peer_backoff(failures, jitter);
        }
}
/* The longest a retry time saved by an earlier run is waited for. */
static inline unsigned ntb_peer_retry_limit(enum ntb_peer_kind kind)
{
        return kind == NTB_PEER_EXPLICIT ? 0 : kind == NTB_PEER_PROVEN ? 300 : 3600;
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
