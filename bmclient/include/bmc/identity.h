// The records the codec reads and returns: an identity (keys included), a borrowed view of one, and
// a received message.
#pragma once
#include <bmc/secret.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace bmc {
// Bitmessage nodes drop any object over 2^18 bytes. A letter's text --
// "Subject:<subject>\nBody:<body>" in UTF-8 -- may take kMaxLetterText of them; the rest is the
// object header, the sender's keys, the acknowledgment, the signature and the encryption: 486 bytes
// at most for a msg (see Wire::encodeMessage), fewer for a broadcast, with some slack on top.
constexpr int kMaxObjectBytes = 262144;
constexpr int kMaxLetterText = 261000;
inline int letterTextBytes(std::string_view subject, std::string_view body) {
    return int(8 + subject.size() + 6 + body.size()); // "Subject:" + subject + "\nBody:" + body
}

struct Identity {
    std::string label, address;
    bool chan = false;
    bool isDefault = false;
    Secret keys{64}; // the signing key, then the encryption key
    Identity() = default;
    Identity(Identity &&) = default;
    Identity &operator=(Identity &&) = default;
};

// An identity as the codec reads it, without copying the keys. Valid while the identity it was made
// from lives.
struct IdentityRef {
    std::string_view address;
    bool chan = false;
    const unsigned char *keys = nullptr; // 64 bytes: the signing key, then the encryption key
    IdentityRef() = default;
    IdentityRef(const Identity &i) : address(i.address), chan(i.chan), keys(i.keys.data()) {}
    IdentityRef(std::string_view address, bool chan, const unsigned char *keys)
        : address(address), chan(chan), keys(keys) {}
};

struct Message {
    std::string hash, from, to, subject, body, folder;
    int64_t received = 0;
};
} // namespace bmc
