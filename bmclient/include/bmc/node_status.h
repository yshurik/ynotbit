// The node's status.json: the counters the app shows and the port the node ended up listening
// on. A file that is missing, half-written or foreign reads as nothing known, and the reader
// keeps whatever it had.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace bmc {
struct NodeStatus {
    int64_t time = 0; // the "time" the node wrote it, seconds since the epoch
    int peers = 0;
    int pending = 0;
    int64_t objects = 0;
    int64_t objectBytes = 0;
    int establishedOutgoing = 0;
    int pendingOutgoing = 0;
    int establishedIncoming = 0;
    int knownAddresses = 0;
    int eligibleAddresses = 0;
    int64_t attempts = 0;
    int64_t setupTimeouts = 0;
    int64_t rejections = 0;
    int listenPort = 0;

    // Nothing when the text is no object, or has no "objects" member at all: that is the app's
    // "the node has not said anything yet". A member of another type reads as zero, as
    // QJsonValue::toInteger does.
    static std::optional<NodeStatus> parse(std::string_view text);
    // The first 4096 bytes of the file; nothing when it cannot be opened.
    static std::optional<NodeStatus> fromFile(const std::filesystem::path &file);
    // The app calls a status this old "unavailable"; the node writes one a second.
    bool fresh(int64_t now, int64_t maxAge = 10) const {
        return time > 0 && now - time <= maxAge;
    }
};
} // namespace bmc
