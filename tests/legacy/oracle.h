// The old code behind plain C++ types, for the parity tests (see scripts/legacy-snapshot.sh).
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oracle {
using Bytes = std::vector<uint8_t>;
struct Id {
    std::string label, address;
    bool chan = false;
    std::array<uint8_t, 64> keys{};
};
struct Pub {
    std::string address;
    Bytes signingKey, encryptionKey;
    uint64_t nonceTrials = 1000, extraBytes = 1000;
    uint32_t behaviors = 1;
    bool chan = false;
};
struct Msg {
    std::string hash, from, to, subject, body, folder;
};
struct Env {
    Msg message;
    Pub sender;
    Bytes acknowledgment;
    bool broadcast = false;
};
struct Header {
    uint32_t type;
    uint64_t version, stream;
    int64_t expires;
    int headerSize;
};

// Protocol
Id fromPrivate(const std::string &label, const Bytes &signing, const Bytes &encryption, int version,
               int stream, bool chan);
Id channel(const std::string &phrase, const std::string &label, int version);
Bytes protocolEncode(const Id &sender, const Id &recipient, const std::string &subject,
                     const std::string &body, int64_t expires);
std::optional<Msg> protocolDecode(const Bytes &object, const Id &recipient);
std::string inventoryHash(const Bytes &object);
// Wire
std::optional<Header> header(const Bytes &object);
bool validAddress(const std::string &address);
Pub publicIdentity(const Id &identity);
Bytes wireEncode(const Id &sender, const Pub &recipient, const std::string &subject,
                 const std::string &body, int64_t expires, const Bytes &ackObject);
std::optional<Env> wireDecode(const Bytes &object, const Id &recipient);
Bytes encodePubkey(const Id &identity, int64_t expires);
std::optional<Pub> decodePubkey(const Bytes &object, const std::string &address);
Bytes getPubkey(const std::string &address, int64_t expires);
bool requestsIdentity(const Bytes &object, const Id &identity);
Bytes acknowledgment(const Bytes &token, int64_t expires);
Bytes acknowledgmentToken(const Bytes &object);
Bytes encodeBroadcast(const Id &identity, const std::string &subject, const std::string &body,
                      int64_t expires);
std::optional<Env> decodeBroadcast(const Bytes &object, const std::string &address);
Bytes frame(const Bytes &object);
// Proof of work
bool powValid(const Bytes &object, int64_t now, uint64_t trials, uint64_t extra);
uint64_t trialValue(uint64_t nonce, const std::array<uint8_t, 64> &initial);
// Phrase keys
Id phraseIdentity(const std::string &phrase, const std::string &label, uint64_t pair);
std::optional<uint64_t> firstPair(const std::string &phrase, int zeroBytes, uint64_t limit);
int zeroBytes(const std::string &address);
} // namespace oracle
