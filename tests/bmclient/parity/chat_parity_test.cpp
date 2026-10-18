#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <bmc/bytes.h>
#include <bmc/chat_format.h>
#include <bmc/test.h>
#include <bmc/text.h>
#include <random>

namespace {
// src/chat_format.cpp as it was before slice 1.
constexpr int kOldMaxChatBodyBytes = 8192;
enum class OldResult { Letter, Chat, InvalidChat };
struct OldPayload {
    qint64 seqno = 0;
    QString body;
};
OldResult oldParse(const QString &subject, const QString &text, OldPayload *out) {
    if (!subject.isEmpty())
        return OldResult::Letter;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return OldResult::Letter;
    const auto object = doc.object();
    if (object.value("app") != "ynotbit" || object.value("type") != "chat")
        return OldResult::Letter;
    const auto seq = object.value("seqno").toInteger();
    const auto body = object.value("body").toString();
    if (object.value("version") != 1 || object.value("format") != "markdown" || seq <= 0 ||
        body.trimmed().isEmpty() || body.toUtf8().size() > kOldMaxChatBodyBytes ||
        !object.value("body").isString())
        return OldResult::InvalidChat;
    if (out)
        *out = {seq, body};
    return OldResult::Chat;
}
QString oldBody(qint64 seqno, const QString &body) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"app", "ynotbit"},
                                                       {"version", 1},
                                                       {"type", "chat"},
                                                       {"seqno", seqno},
                                                       {"format", "markdown"},
                                                       {"body", body}})
                                 .toJson(QJsonDocument::Compact));
}
std::string hexOf(const std::string &s) {
    return bmc::hex(bmc::view(s));
}
// What a received text goes through: bytes, decoded as QString::fromUtf8 did.
void same(const std::string &bytes) {
    const QString text = QString::fromUtf8(bytes.data(), qsizetype(bytes.size()));
    OldPayload old;
    const auto expected = oldParse({}, text, &old);
    bmc::ChatPayload payload;
    const auto actual = bmc::parseChatMessage("", bmc::text::sanitizeUtf8(bytes), &payload);
    bool equal = int(expected) == int(actual);
    if (equal && actual == bmc::ChatParseResult::Chat)
        equal = old.seqno == payload.seqno && old.body.toUtf8().toStdString() == payload.body;
    if (!equal)
        bmc::test::fail(__FILE__, __LINE__,
                        "parseChatMessage differs for " + hexOf(bytes) + ": Qt " +
                            std::to_string(int(expected)) + ", core " +
                            std::to_string(int(actual)));
}
} // namespace

TEST(chat_message_body_matches_the_old_writer) {
    std::mt19937_64 rng(0x5eed0005);
    const char *pieces[] = {"a",
                            "hello",
                            " ",
                            "\n",
                            "\t",
                            "\"",
                            "\\",
                            "/",
                            "\xc3\xa9",
                            "\xd0\x9f",
                            "\xf0\x9f\x98\x80",
                            "\xe2\x80\xa8",
                            "\x7f",
                            "\x01",
                            "\xc2\xa0"};
    for (int round = 0; round < 20000; ++round) {
        std::string body;
        for (int i = int(rng() % 8); i > 0; --i)
            body += pieces[rng() % std::size(pieces)];
        const qint64 seqno = qint64(rng() % 3 ? rng() % 1000 : rng() >> 1);
        const auto expected =
            oldBody(seqno, QString::fromUtf8(body.data(), qsizetype(body.size())));
        CHECK_EQ(bmc::chatMessageBody(seqno, body), expected.toUtf8().toStdString());
    }
}

TEST(chat_parse_matches_qt_on_a_grid_of_documents) {
    const char *versions[] = {"1", "1.0", "1e0", "\"1\"", "2", "true", "null", "0"};
    const char *formats[] = {"\"markdown\"", "\"html\"", "\"Markdown\"", "1", "null", "\"\""};
    const char *seqnos[] = {
        "1",  "0",    "-5", "2.5", "3.0", "1e2", "9223372036854775807", "9223372036854775808",
        ".5", "\"7\""};
    const char *bodies[] = {"\"x\"", "\"\"", "\"   \"",     "\"\\u00a0\"", "\"\\u3000x\"",
                            "5",     "null", "\"\\ud83d\"", "\"a\\nb\"",   "\"\xc3\xa9\"",
                            "[]"};
    for (const char *version : versions)
        for (const char *format : formats)
            for (const char *seqno : seqnos)
                for (const char *body : bodies)
                    same(std::string("{\"app\":\"ynotbit\",\"type\":\"chat\",\"version\":") +
                         version + ",\"format\":" + format + ",\"seqno\":" + seqno +
                         ",\"body\":" + body + "}");
}

TEST(chat_parse_matches_qt_on_mutated_chats) {
    std::mt19937_64 rng(0x5eed0006);
    const std::string seeds[] = {
        oldBody(1, "Hi **Alice**").toUtf8().toStdString(),
        oldBody(1760000000123, "line\nbreak \"quoted\" \\ \xf0\x9f\x98\x80").toUtf8().toStdString(),
        oldBody(5, "  padded  ").toUtf8().toStdString(),
    };
    const std::string pool =
        "{}[]:,\"\\ \n\t\f.-+eE0123456789tfnulsa/u\xc3\xa9\xf0\x9f\x98\x80\xff\x01\xef\xbb\xbf";
    for (int round = 0; round < 150000; ++round) {
        std::string text = seeds[rng() % std::size(seeds)];
        for (int e = 1 + int(rng() % 3); e > 0 && !text.empty(); --e) {
            const size_t at = rng() % text.size();
            switch (rng() % 4) {
            case 0:
                text.erase(at, 1 + rng() % 3);
                break;
            case 1:
                text.insert(at, 1, pool[rng() % pool.size()]);
                break;
            case 2:
                text[at] = pool[rng() % pool.size()];
                break;
            default:
                text.resize(at);
                break;
            }
        }
        same(text);
    }
}
