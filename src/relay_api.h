#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "ntb-peer-stats.h"
struct ntb_network;
void ntb_network_tick(struct ntb_network *);
void ntb_network_get_peer_stats(struct ntb_network *, struct ntb_network_peer_stats *);
int ntb_network_connected_peers(struct ntb_network *);
int ntb_network_pending_objects(struct ntb_network *);
int ntb_network_submit(struct ntb_network *, const uint8_t *, size_t);
void ntb_network_offer(struct ntb_network *, const uint8_t *);
#ifdef __cplusplus
}
#endif
