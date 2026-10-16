#include "config.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ntb-socket.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
/* A peer that vanishes without a word (a laptop that slept, a router that forgot the flow) is
 * found out by the system in a couple of minutes, not by the node's ten minutes of silence. */
int main(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(sock != -1);
    int on = -1;
    socklen_t len = sizeof on;
    CHECK(getsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, &len) == 0 && on == 0);
    ntb_socket_set_keepalive(sock);
    len = sizeof on;
    CHECK(getsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, &len) == 0 && on != 0);
    int idle = 0, interval = 0, count = 1;
#if defined(TCP_KEEPIDLE)
    len = sizeof idle;
    CHECK(getsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, &len) == 0);
#elif defined(TCP_KEEPALIVE)
    len = sizeof idle;
    CHECK(getsockopt(sock, IPPROTO_TCP, TCP_KEEPALIVE, &idle, &len) == 0);
#endif
#if defined(TCP_KEEPINTVL)
    len = sizeof interval;
    CHECK(getsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, &len) == 0);
#endif
#if defined(TCP_KEEPCNT)
    len = sizeof count;
    CHECK(getsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, &len) == 0);
#endif
    CHECK(idle > 0 && idle <= 120);
    CHECK(interval > 0 && interval <= 30);
    CHECK(count > 0 && idle + interval * count <= 240);
    close(sock);
    return 0;
}
