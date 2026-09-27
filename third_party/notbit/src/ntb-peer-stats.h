#ifndef NTB_PEER_STATS_H
#define NTB_PEER_STATS_H
#include <stdint.h>
struct ntb_network_peer_stats {
        unsigned established_outgoing, pending_outgoing, established_incoming;
        unsigned known_addresses, eligible_addresses;
        uint64_t attempts, setup_timeouts, rejections;
};
#endif
