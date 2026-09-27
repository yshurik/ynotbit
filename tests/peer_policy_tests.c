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
    const int64_t now = 2000000000;
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
