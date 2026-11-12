// A chat on the wire: a msg with an empty subject whose text is a small JSON document.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace bmc {
constexpr int64_t kChatTtlSeconds = 86400;

struct ChatPayload {
    int64_t seqno = 0;
    std::string body;
};
enum class ChatParseResult { Letter, Chat, InvalidChat };

// A chat has no limit of its own. It is a msg with no subject whose text is the JSON of
// chatMessageBody(), so it may be as long as a letter's text (kMaxLetterText, identity.h): what an
// object of 2^18 bytes holds less its header, the sender's keys, the acknowledgment, the signature
// and the encryption. True when a chat with this body fits, counted as it goes on the wire: the
// body as JSON writes it (a quote, a backslash and a line break are two bytes, another control
// character six) in the JSON around it, with the longest sequence number, in a text with no
// subject. Throws, as chatMessageBody() does, when the body is no UTF-8.
bool chatFits(std::string_view body);

// {"app":"ynotbit","body":...,"format":"markdown","seqno":N,"type":"chat","version":1}, compact,
// keys in this order (as Qt wrote it). The body must be valid UTF-8.
std::string chatMessageBody(int64_t seqno, std::string_view body);
// Letter: not a chat (a subject, or not our JSON). InvalidChat: ours, but wrong. Chat: fills *out.
// `text` is the message text after text::sanitizeUtf8.
ChatParseResult parseChatMessage(std::string_view subject, std::string_view text, ChatPayload *out);
} // namespace bmc
