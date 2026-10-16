#include <stdio.h>
#include "ntb-peer-policy.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    CHECK(!ntb_peer_setup_expired(19000000, 0, false));
    CHECK(ntb_peer_setup_expired(20000000, 0, false));
    CHECK(!ntb_peer_setup_expired(59000000, 0, true));
    CHECK(ntb_peer_setup_expired(60000000, 0, true));
    CHECK(ntb_peer_backoff(1, 0) == 60);
    CHECK(ntb_peer_backoff(2, 0) == 120);
    CHECK(ntb_peer_backoff(999, 0) == 3600);
    CHECK(ntb_peer_backoff(999, 59) == 3600);
    /* A peer the operator named is a choice, not a guess: it is tried again in seconds, for
     * as long as it takes, and never waits the hour a stale public address does. */
    CHECK(ntb_peer_backoff_kind(NTB_PEER_EXPLICIT, 1, 0) == 5);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_EXPLICIT, 2, 0) == 10);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_EXPLICIT, 3, 0) == 20);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_EXPLICIT, 4, 0) == 30);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_EXPLICIT, 999, 0) == 30);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_EXPLICIT, 999, 99) <= 34);
    /* A peer that has worked is worth coming back to: minutes, not an hour. */
    CHECK(ntb_peer_backoff_kind(NTB_PEER_PROVEN, 1, 0) == 60);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_PROVEN, 2, 0) == 120);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_PROVEN, 3, 0) == 240);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_PROVEN, 4, 0) == 300);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_PROVEN, 999, 15) == 300);
    /* Everything else keeps the public schedule. */
    CHECK(ntb_peer_backoff_kind(NTB_PEER_DISCOVERED, 1, 0) == 60);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_DISCOVERED, 7, 0) == 3600);
    CHECK(ntb_peer_backoff_kind(NTB_PEER_DISCOVERED, 999, 59) == 3600);
    /* A saved retry time is waited for no longer than the kind's own backoff allows. */
    CHECK(ntb_peer_retry_limit(NTB_PEER_EXPLICIT) == 0);
    CHECK(ntb_peer_retry_limit(NTB_PEER_PROVEN) == 300);
    CHECK(ntb_peer_retry_limit(NTB_PEER_DISCOVERED) == 3600);
    const int64_t now = 2000000000;
    CHECK(ntb_peer_proven(now, now - 1));
    CHECK(ntb_peer_proven(now, now - 27 * 86400));
    CHECK(!ntb_peer_proven(now, now - 28 * 86400));
    CHECK(!ntb_peer_proven(now, 0));
    CHECK(!ntb_peer_proven(now, now + 5));
    CHECK(ntb_peer_retained(now, now-7500, 0));
    CHECK(ntb_peer_retained(now, now-86400, now-86400));
    CHECK(!ntb_peer_retained(now, now-172800, 0));
    CHECK(!ntb_peer_retained(now, now-29*86400, now-29*86400));
    CHECK(!ntb_peer_retained(now, -1, -1));
    CHECK(!ntb_peer_should_ping(299000000, 0, 0));
    CHECK(ntb_peer_should_ping(300000000, 0, 0));
    CHECK(!ntb_peer_should_ping(301000000, 0, 300000000));
    return 0;
}
