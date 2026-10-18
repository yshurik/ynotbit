#include "chat_format.h"
#include "qt_bridge.h"
#include <bmc/chat_format.h>

namespace bm {
static_assert(kChatTtlSeconds == bmc::kChatTtlSeconds &&
              kMaxChatBodyBytes == bmc::kMaxChatBodyBytes);

QString chatMessageBody(qint64 seqno, const QString &body) {
    return bridge::fromStd(bmc::chatMessageBody(seqno, bridge::toStd(body)));
}

ChatParseResult parseChatMessage(const QString &subject, const QString &text, ChatPayload *out) {
    bmc::ChatPayload payload;
    const auto result =
        bmc::parseChatMessage(bridge::toStd(subject), bridge::toStd(text), &payload);
    if (result == bmc::ChatParseResult::Chat && out)
        *out = {payload.seqno, bridge::fromStd(payload.body)};
    return static_cast<ChatParseResult>(result);
}
} // namespace bm
