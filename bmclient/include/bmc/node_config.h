// What the node is started with: the settings the app keeps in desktop.ini, and the argv they
// make. A port of 0 is "not asked for"; a peer or proxy is IP:port, or empty for none.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bmc {
constexpr int kNodePort = 8444; // the node's NTB_PROTO_DEFAULT_PORT

struct NodeConfig {
    std::string root;       // -D
    std::string maildir;    // -m
    bool listen = true;     // -i when false
    int retentionMB = 2048; // -R, clamped to 64..32768
    int retentionDays = 90; // -A, clamped to 1..3650
    int maxOutgoing = 0;    // -o when above zero
    int maxIncoming = 0;    // -n when above zero
    int port = 0;           // -p when above zero, with listenPort()'s value
    std::string peer;       // -P and -L when set
    std::string proxy;      // -r and -B when set
    std::string bin;        // the executable to run; not part of the argv

    // The port the node is asked for, and the one the app shows when it has nothing better: a
    // port of 0, or one outside 1..65535, means the node's own default.
    int listenPort() const {
        return port > 0 && port < 65536 ? port : kNodePort;
    }
    void clampRetention();
    // The argv the app builds today, --node first.
    std::vector<std::string> nodeArguments() const;
};

// An empty string is the "no peer" the app accepts; anything else must be IP:port, as -P and -r
// take it: an address that parses, a port in 1..65535, no user info, path, query or fragment.
// Surrounding whitespace is trimmed first (the app trims the value it saves; Qt trims the leading
// whitespace too, but not the trailing one, which is the one difference).
bool validEndpoint(std::string_view text);
// The node's port is opened on the router only when the node listens with no proxy in front of it.
bool wantsPortMapping(bool online, bool listen, bool upnp, std::string_view proxy);
} // namespace bmc
