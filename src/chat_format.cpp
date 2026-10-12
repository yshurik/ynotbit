#include "chat_format.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
namespace bm {
QString chatMessageBody(qint64 seqno, const QString &body) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"app", "ynotbit"},
                                                   {"version", 1},
                                                   {"type", "chat"},
                                                   {"seqno", seqno},
                                                   {"format", "markdown"},
                                                   {"body", body}})
                                 .toJson(QJsonDocument::Compact));
}
ChatParseResult parseChatMessage(const QString &subject, const QString &text, ChatPayload *out) {
    if (!subject.isEmpty())
        return ChatParseResult::Letter;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return ChatParseResult::Letter;
    const auto object = doc.object();
    if (object.value("app") != "ynotbit" || object.value("type") != "chat")
        return ChatParseResult::Letter;
    const auto seq = object.value("seqno").toInteger();
    const auto body = object.value("body").toString();
    if (object.value("version") != 1 || object.value("format") != "markdown" || seq <= 0 ||
        body.trimmed().isEmpty() || body.toUtf8().size() > kMaxChatBodyBytes ||
        !object.value("body").isString())
        return ChatParseResult::InvalidChat;
    if (out)
        *out = {seq, body};
    return ChatParseResult::Chat;
}
}
