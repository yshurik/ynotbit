// The node's settings and the argv they make: a move of Session::nodeArguments(),
// Session::configuredPort(), Session::validEndpoint() and Session::wantsPortMapping(), with the
// Qt answers kept (see node_config_test.cpp for the strings Qt accepted and refused).
#include <algorithm>
#include <bmc/node_config.h>
#include <cctype>
#include <utility>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace bmc {
namespace {
bool isIpv4(std::string_view host) {
    int parts = 0;
    size_t start = 0;
    while (start <= host.size()) {
        const auto dot = host.find('.', start);
        const auto part = host.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                                           : dot - start);
        if (part.empty() || part.size() > 3)
            return false;
        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + (c - '0');
        }
        if (value > 255)
            return false;
        ++parts;
        if (dot == std::string_view::npos)
            break;
        start = dot + 1;
    }
    return parts == 4;
}

bool isIpv6(std::string_view host) {
    if (host.find(':') == std::string_view::npos)
        return false;
    const std::string text(host);
    unsigned char parsed[16];
    return inet_pton(AF_INET6, text.c_str(), parsed) == 1;
}

// A port of leading zeros reads as the number, as QUrl reads it; anything but digits, an empty
// string or a number above 65535 is no port at all.
bool isPort(std::string_view text) {
    if (text.empty())
        return false;
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + (c - '0');
        if (value > 65535)
            return false;
    }
    return value > 0;
}
} // namespace

void NodeConfig::clampRetention() {
    retentionMB = std::clamp(retentionMB, 64, 32768);
    retentionDays = std::clamp(retentionDays, 1, 3650);
}

std::vector<std::string> NodeConfig::nodeArguments() const {
    std::vector<std::string> args{"--node", "-D", root, "-m", maildir};
    if (!listen)
        args.emplace_back("-i");
    args.emplace_back("-R");
    args.emplace_back(std::to_string(retentionMB));
    args.emplace_back("-A");
    args.emplace_back(std::to_string(retentionDays));
    // A service asks for more peers (the node's own are eight of each) and a port of its own.
    for (const auto &[count, option] : {std::pair{maxOutgoing, "-o"}, {maxIncoming, "-n"}})
        if (count > 0) {
            args.emplace_back(option);
            args.emplace_back(std::to_string(count));
        }
    if (port > 0) {
        args.emplace_back("-p");
        args.emplace_back(std::to_string(listenPort()));
    }
    if (!peer.empty()) {
        args.emplace_back("-P");
        args.emplace_back(peer);
        args.emplace_back("-L");
    }
    if (!proxy.empty()) {
        args.emplace_back("-r");
        args.emplace_back(proxy);
        args.emplace_back("-B");
    }
    // A private bot: the node links to its peer alone. Last, so every other row is unchanged.
    if (linkOnly)
        for (const char *flag : {"-b", "-B", "-e"})
            args.emplace_back(flag);
    return args;
}

bool validEndpoint(std::string_view text) {
    // Exactly empty is the app's "no peer"; whitespace alone is not an endpoint, as Qt's parse of
    // "tcp:// " is not either.
    if (text.empty())
        return true;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    if (text.empty())
        return false;
    for (const char c : text)
        if (std::isspace(static_cast<unsigned char>(c)) || c == '@' || c == '/' || c == '?' ||
            c == '#')
            return false;
    std::string_view port;
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos || !isIpv6(text.substr(1, close - 1)))
            return false;
        const auto rest = text.substr(close + 1);
        if (rest.size() < 2 || rest.front() != ':')
            return false;
        port = rest.substr(1);
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos || !isIpv4(text.substr(0, colon)))
            return false;
        port = text.substr(colon + 1);
    }
    return isPort(port);
}

bool wantsPortMapping(bool online, bool listen, bool upnp, std::string_view proxy) {
    return online && listen && upnp && proxy.empty();
}
} // namespace bmc
