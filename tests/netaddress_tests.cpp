extern "C" {
#include "ntb-netaddress.h"
}
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main() {
    try {
        // IPv4 string round-trip.
        ntb_netaddress v4{};
        require(ntb_netaddress_from_string(&v4, "1.2.3.4:8444", 0), "parse ipv4");
        require(!ntb_netaddress_is_ipv6(&v4), "ipv4 not detected as ipv6");
        char *v4str = ntb_netaddress_to_string(&v4);
        require(std::strcmp(v4str, "1.2.3.4:8444") == 0, "ipv4 to_string round-trip");
        free(v4str);

        // Default port applies when none is given.
        ntb_netaddress v4NoPort{};
        require(ntb_netaddress_from_string(&v4NoPort, "5.6.7.8", 12345), "parse ipv4 no port");
        require(v4NoPort.port == 12345, "default port applied");

        // IPv6 string round-trip.
        ntb_netaddress v6{};
        require(ntb_netaddress_from_string(&v6, "[::1]:8444", 0), "parse ipv6");
        require(ntb_netaddress_is_ipv6(&v6), "ipv6 detected as ipv6");
        char *v6str = ntb_netaddress_to_string(&v6);
        require(std::strcmp(v6str, "[::1]:8444") == 0, "ipv6 to_string round-trip");
        free(v6str);

        // Native sockaddr round-trip (this is what needed the Winsock2 port on
        // Windows: struct sockaddr_in/sockaddr_in6, AF_INET/AF_INET6, htons/ntohs).
        ntb_netaddress_native nativeV4;
        ntb_netaddress_to_native(&v4, &nativeV4);
        require(nativeV4.sockaddr_in.sin_family == AF_INET, "native ipv4 family");
        require(nativeV4.length == sizeof(nativeV4.sockaddr_in), "native ipv4 length");
        ntb_netaddress v4FromNative{};
        ntb_netaddress_from_native(&v4FromNative, &nativeV4);
        require(std::memcmp(v4FromNative.host, v4.host, sizeof v4.host) == 0,
                "ipv4 native round-trip host");
        require(v4FromNative.port == v4.port, "ipv4 native round-trip port");

        ntb_netaddress_native nativeV6;
        ntb_netaddress_to_native(&v6, &nativeV6);
        require(nativeV6.sockaddr_in6.sin6_family == AF_INET6, "native ipv6 family");
        require(nativeV6.length == sizeof(nativeV6.sockaddr_in6), "native ipv6 length");
        ntb_netaddress v6FromNative{};
        ntb_netaddress_from_native(&v6FromNative, &nativeV6);
        require(std::memcmp(v6FromNative.host, v6.host, sizeof v6.host) == 0,
                "ipv6 native round-trip host");
        require(v6FromNative.port == v6.port, "ipv6 native round-trip port");

        // Address-class filtering.
        ntb_netaddress loopback{};
        ntb_netaddress_from_string(&loopback, "127.0.0.1:8444", 0);
        require(!ntb_netaddress_is_allowed(&loopback, true), "loopback never allowed");

        ntb_netaddress priv10{};
        ntb_netaddress_from_string(&priv10, "10.0.0.1:8444", 0);
        require(!ntb_netaddress_is_allowed(&priv10, false), "10.x rejected by default");
        require(ntb_netaddress_is_allowed(&priv10, true), "10.x allowed when opted in");

        ntb_netaddress pub{};
        ntb_netaddress_from_string(&pub, "8.8.8.8:8444", 0);
        require(ntb_netaddress_is_allowed(&pub, false), "public address allowed");

        // Malformed input is rejected, not crashed on.
        ntb_netaddress bad{};
        require(!ntb_netaddress_from_string(&bad, "not-an-address", 0), "garbage rejected");
        require(!ntb_netaddress_from_string(&bad, "1.2.3.4:not-a-port", 0), "bad port rejected");
        require(!ntb_netaddress_from_string(&bad, "[::1", 0), "unterminated ipv6 rejected");

        std::cout << "PASS: ipv4/ipv6 string and native sockaddr round-trip, address-class "
                     "filtering, malformed input rejection\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
