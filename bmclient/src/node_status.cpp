// The status.json the relay bridge writes, read back without Qt: the keys of
// src/relay_bridge.cpp's status object, one for one.
#include <bmc/json.h>
#include <bmc/node_status.h>
#include <fstream>

namespace bmc {
namespace {
int counter(const json::Value &document, std::string_view key) {
    return int(document.at(key).toInteger());
}
} // namespace

std::optional<NodeStatus> NodeStatus::parse(std::string_view text) {
    const auto document = json::parse(text);
    if (!document || !document->isObject() || document->at("objects").isUndefined())
        return std::nullopt;
    NodeStatus status;
    status.time = document->at("time").toInteger();
    status.peers = counter(*document, "peers");
    status.pending = counter(*document, "pending");
    status.objects = document->at("objects").toInteger();
    status.objectBytes = document->at("object_bytes").toInteger();
    status.establishedOutgoing = counter(*document, "established_outgoing");
    status.pendingOutgoing = counter(*document, "pending_outgoing");
    status.establishedIncoming = counter(*document, "established_incoming");
    status.knownAddresses = counter(*document, "known_addresses");
    status.eligibleAddresses = counter(*document, "eligible_addresses");
    status.attempts = document->at("attempts").toInteger();
    status.setupTimeouts = document->at("setup_timeouts").toInteger();
    status.rejections = document->at("rejections").toInteger();
    status.listenPort = counter(*document, "listen_port");
    return status;
}

std::optional<NodeStatus> NodeStatus::fromFile(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string text(4096, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(in.gcount()));
    return parse(text);
}
} // namespace bmc
