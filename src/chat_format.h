#pragma once
#include <QString>
namespace bm {
constexpr qint64 kChatTtlSeconds = 86400;
constexpr int kMaxChatBodyBytes = 8192;
struct ChatPayload {
    qint64 seqno = 0;
    QString body;
};
enum class ChatParseResult { Letter, Chat, InvalidChat };
QString chatMessageBody(qint64 seqno, const QString &body);
ChatParseResult parseChatMessage(const QString &subject, const QString &text, ChatPayload *out);
}
