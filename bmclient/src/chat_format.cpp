#include <bmc/chat_format.h>
#include <bmc/error.h>
#include <bmc/json.h>
#include <bmc/text.h>

namespace bmc {
std::string chatMessageBody(int64_t seqno, std::string_view body) {
    if (!text::validUtf8(body))
        throw Error(Code::Argument, "Chat text must be valid UTF-8");
    return std::string(R"({"app":"ynotbit","body":)") + json::quote(body) +
           R"(,"format":"markdown","seqno":)" + std::to_string(seqno) +
           R"(,"type":"chat","version":1})";
}

ChatParseResult parseChatMessage(std::string_view subject, std::string_view text,
                                 ChatPayload *out) {
    if (!subject.empty())
        return ChatParseResult::Letter;
    const auto doc = json::parse(text);
    if (!doc || !doc->isObject())
        return ChatParseResult::Letter;
    if (!doc->at("app").equalsString("ynotbit") || !doc->at("type").equalsString("chat"))
        return ChatParseResult::Letter;
    const auto seq = doc->at("seqno").toInteger();
    const auto body = doc->at("body").toString();
    if (!doc->at("version").equalsNumber(1) || !doc->at("format").equalsString("markdown") ||
        seq <= 0 || text::isBlank(body) || body.size() > size_t(kMaxChatBodyBytes) ||
        !doc->at("body").isString())
        return ChatParseResult::InvalidChat;
    if (out)
        *out = {seq, body};
    return ChatParseResult::Chat;
}
} // namespace bmc
