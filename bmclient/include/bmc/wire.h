// The objects of the Bitmessage network the app sends and reads: msg, pubkey, getpubkey, broadcast,
// the acknowledgment and the network frame; ECIES as pyelliptic does it, ECDSA signatures.
#pragma once
#include <bmc/bytes.h>
#include <bmc/identity.h>
#include <optional>
#include <string>
#include <string_view>

namespace bmc {
struct PublicIdentity {
    std::string address;
    Bytes signingKey, encryptionKey; // 65-byte uncompressed points
    uint64_t nonceTrials = 1000, extraBytes = 1000;
    uint32_t behaviors = 1;
    bool chan = false;
};
struct ObjectHeader {
    uint32_t type;
    uint64_t version, stream;
    int64_t expires;
    int headerSize;
};
struct DecodedEnvelope {
    Message message;
    PublicIdentity sender;
    Bytes acknowledgment;
    bool broadcast = false;
};

class Wire {
  public:
    static std::optional<ObjectHeader> header(ByteView object);
    static bool validAddress(std::string_view address);
    static PublicIdentity publicIdentity(IdentityRef identity);
    // The text is "Subject:<subject>\nBody:<body>" in UTF-8; throws on a subject with a line break,
    // text that is no UTF-8, a letter over kMaxLetterText, keys that do not fit their address.
    static Bytes encodeMessage(IdentityRef sender, const PublicIdentity &recipient,
                               std::string_view subject, std::string_view body, int64_t expires,
                               ByteView ackObject = {});
    // The same with the text as bytes, put on the wire as they are (encoding 2).
    static Bytes encodeMessageText(IdentityRef sender, const PublicIdentity &recipient,
                                   ByteView text, int64_t expires, ByteView ackObject = {});
    static std::optional<DecodedEnvelope> decodeMessage(ByteView object, IdentityRef recipient);
    static Bytes encodePubkey(IdentityRef identity, int64_t expires);
    static std::optional<PublicIdentity> decodePubkey(ByteView object, std::string_view address);
    static Bytes getPubkey(std::string_view address, int64_t expires);
    static bool requestsIdentity(ByteView object, IdentityRef identity);
    static Bytes acknowledgment(ByteView token, int64_t expires);
    // The 32-byte token of an acknowledgment object; empty if the object is none.
    static Bytes acknowledgmentToken(ByteView object);
    static Bytes encodeBroadcast(IdentityRef identity, std::string_view subject,
                                 std::string_view body, int64_t expires);
    static std::optional<DecodedEnvelope> decodeBroadcast(ByteView object,
                                                          std::string_view address);
    static Bytes frame(ByteView object);
};
} // namespace bmc
