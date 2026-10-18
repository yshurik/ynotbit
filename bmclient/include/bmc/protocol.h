// Identities and the simple message of the engine the app started from ("legacy notbit"): kept
// because old objects and tests use it. Wire (wire.h) is what the app sends today.
#pragma once
#include <bmc/bytes.h>
#include <bmc/identity.h>
#include <optional>
#include <string>
#include <string_view>

namespace bmc {
class Protocol {
  public:
    // Random keys, tried until the address has a zero RIPE byte (a shorter address).
    static Identity identity(std::string_view label);
    // A chan's keys from its phrase, as PyBitmessage derives them (address version 3 or 4).
    static Identity channel(std::string_view phrase, std::string_view label, int version = 4);
    static Identity fromPrivate(std::string_view label, ByteView signing, ByteView encryption,
                                int version = 4, int stream = 1, bool chan = false);
    static Bytes encodeMessage(IdentityRef sender, IdentityRef recipient, std::string_view subject,
                               std::string_view body, int64_t expires);
    static std::optional<Message> decodeMessage(ByteView object, IdentityRef recipient);
    // Lower-case hex of the first 32 bytes of SHA-512(SHA-512(object)).
    static std::string inventoryHash(ByteView object);
};
} // namespace bmc
