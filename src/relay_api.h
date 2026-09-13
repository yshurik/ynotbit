#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ntb_network;
int ntb_network_connected_peers(struct ntb_network *);
int ntb_network_submit(struct ntb_network *, const uint8_t *, size_t);
void ntb_network_offer(struct ntb_network *, const uint8_t *);
#ifdef __cplusplus
}
#endif
