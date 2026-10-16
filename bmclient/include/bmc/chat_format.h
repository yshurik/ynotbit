// A chat on the wire: a msg with an empty subject whose text is a small JSON document.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace bmc {
constexpr int64_t kChatTtlSeconds = 86400;
constexpr int kMaxChatBodyBytes = 8192;

struct ChatPayload {
    int64_t seqno = 0;
    std::string body;
};
enum class ChatParseResult { Letter, Chat, InvalidChat };

// {"app":"ynotbit","body":...,"format":"markdown","seqno":N,"type":"chat","version":1}, compact,
// keys in this order (as Qt wrote it). The body must be valid UTF-8.
std::string chatMessageBody(int64_t seqno, std::string_view body);
// Letter: not a chat (a subject, or not our JSON). InvalidChat: ours, but wrong. Chat: fills *out.
// `text` is the message text after text::sanitizeUtf8.
ChatParseResult parseChatMessage(std::string_view subject, std::string_view text, ChatPayload *out);
} // namespace bmc
