// The records the storage layer keeps, in std types. Identity and Message live in bmc/identity.h.
#pragma once
#include <bmc/bytes.h>
#include <bmc/identity.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace bmc {
// Drafts stay on this device, so they may run well past what the network carries.
constexpr int kMaxDraftBytes = 4 * 1024 * 1024;

struct ChatMessage {
    std::string id, localAddress, peerAddress, body, state;
    int64_t seqno = 0, received = 0, expires = 0;
    bool outgoing = false;
};
// A conversation as a list shows it: its newest message and what is unread.
struct ChatSummary {
    std::string local, peer, body, state;
    int64_t received = 0;
    bool outgoing = false;
    int unread = 0;
};
struct OutboxItem {
    std::string id, kind, state, error, objectHash;
    Bytes ackToken, ackObject;
    int64_t expires = 0, nextAttempt = 0;
    int attempts = 0;
    Message message;
    bool chat = false;
};
struct NetworkJob {
    std::string id, owner, kind, address, state, hash, error;
    Bytes payload;
    int64_t expires = 0;
    uint64_t nonceTrials = 1000, extraBytes = 1000;
};
struct StoredPublicKey {
    std::string address;
    Bytes signingKey, encryptionKey;
    uint64_t nonceTrials = 1000, extraBytes = 1000;
    uint32_t behaviors = 1;
    int64_t expires = 0;
};
struct Subscription {
    std::string address, label;
};
struct Contact {
    std::string address, label;
    int64_t added = 0;
};
struct DeliveryEvent {
    int64_t timestamp = 0;
    std::string state, detail;
};

// A chan's name as the app shows it: "#<name>". PyBitmessage's default "[chan] <phrase>" and a
// bare "<name>" both become "#<name>".
std::string channelName(std::string_view label);
} // namespace bmc
