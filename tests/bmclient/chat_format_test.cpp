#include <bmc/chat_format.h>
#include <bmc/test.h>

using bmc::ChatParseResult;

namespace {
ChatParseResult parse(const std::string &text, bmc::ChatPayload *out = nullptr,
                      const std::string &subject = "") {
    return bmc::parseChatMessage(subject, text, out);
}
std::string chat(const std::string &version, const std::string &format, const std::string &seqno,
                 const std::string &body) {
    return "{\"app\":\"ynotbit\",\"type\":\"chat\",\"version\":" + version +
           ",\"format\":" + format + ",\"seqno\":" + seqno + ",\"body\":" + body + "}";
}
} // namespace

TEST(chat_body_is_compact_json_with_sorted_keys) {
    CHECK_EQ(
        bmc::chatMessageBody(1, "hi"),
        R"({"app":"ynotbit","body":"hi","format":"markdown","seqno":1,"type":"chat","version":1})");
    CHECK_EQ(bmc::chatMessageBody(1760000000123, "q\"\\\n\xc3\xa9"),
             "{\"app\":\"ynotbit\",\"body\":\"q\\\"\\\\\\n\xc3\xa9\",\"format\":\"markdown\","
             "\"seqno\":1760000000123,\"type\":\"chat\",\"version\":1}");
    CHECK_EQ(bmc::kChatTtlSeconds, 86400);
    CHECK_EQ(bmc::kMaxChatBodyBytes, 8192);
}

TEST(chat_parse_classifies_documents) {
    bmc::ChatPayload payload;
    CHECK(parse(bmc::chatMessageBody(1, "Hi **Alice**"), &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.seqno, 1);
    CHECK_EQ(payload.body, "Hi **Alice**");
    CHECK(parse(bmc::chatMessageBody(1760000000123, "clock"), &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.seqno, 1760000000123);
    CHECK(parse(bmc::chatMessageBody(1, "x"), nullptr, "subject") == ChatParseResult::Letter);
    CHECK(parse("ordinary letter") == ChatParseResult::Letter);
    CHECK(parse("[1]") == ChatParseResult::Letter);
    CHECK(parse("") == ChatParseResult::Letter);
    CHECK(parse(R"({"app":"ynotbit","type":"chat"})") == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"x\"").replace(8, 7, "other")) ==
          ChatParseResult::Letter); // another app
    CHECK(
        parse(
            R"({"app":"ynotbit","type":"letter","version":1,"format":"markdown","seqno":1,"body":"x"})") ==
        ChatParseResult::Letter);
}

TEST(chat_parse_rejects_malformed_chats) {
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"x\"")) == ChatParseResult::Chat);
    CHECK(parse(chat("1.0", "\"markdown\"", "3.0", "\"x\"")) == ChatParseResult::Chat);
    CHECK(parse(chat("2", "\"markdown\"", "1", "\"x\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("\"1\"", "\"markdown\"", "1", "\"x\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"html\"", "1", "\"x\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "0", "\"x\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "-1", "\"x\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "2.5", "\"x\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"   \"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"\\u00a0\\u3000\"")) ==
          ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"\"")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "5")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "null")) == ChatParseResult::InvalidChat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"" + std::string(8192, 'x') + "\"")) ==
          ChatParseResult::Chat);
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"" + std::string(8193, 'x') + "\"")) ==
          ChatParseResult::InvalidChat);
    std::string accents; // the limit is bytes, not characters
    for (int i = 0; i < 4097; ++i)
        accents += "\xc3\xa9";
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"" + accents + "\"")) ==
          ChatParseResult::InvalidChat);
}

TEST(chat_parse_reads_documents_the_way_qt_did) {
    bmc::ChatPayload payload;
    CHECK(parse("\xef\xbb\xbf" + bmc::chatMessageBody(4, "bom"), &payload) ==
          ChatParseResult::Chat);
    CHECK_EQ(payload.seqno, 4);
    CHECK(
        parse(
            R"({"app":"ynotbit","type":"chat","version":1,"format":"markdown","seqno":1,"seqno":9,"body":"dup"})",
            &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.seqno, 9); // the last duplicate wins
    CHECK(parse("{\"app\":\"ynotbit\",\"type\":\"chat\",\"version\":1,\"format\":\"markdown\","
                "\"seqno\":1,\"body\":\"a\nb\"}",
                &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.body, "a\nb"); // a raw line feed in a string
    CHECK(
        parse(
            R"({"app":"ynotbit","type":"chat","version":1,"format":"markdown","seqno":1,"body":"\ud83d"})",
            &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.body, "?"); // a lone surrogate
    CHECK(
        parse(
            R"({"app":"ynotbit","type":"chat","version":1,"format":"markdown","seqno":.5,"body":"x"})") ==
        ChatParseResult::InvalidChat);
    const std::string body = "Привет, **мир**! \xf0\x9f\x98\x80\n\ttab \"quote\" \\ slash";
    CHECK(parse(bmc::chatMessageBody(77, body), &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.body, body);
    CHECK_EQ(payload.seqno, 77);
}

TEST(a_chat_body_that_is_not_utf8_is_refused) {
    CHECK_THROWS(bmc::chatMessageBody(1, "\xff"));
    CHECK_THROWS(bmc::chatMessageBody(1, std::string("ok\xff", 3)));
    CHECK(!bmc::chatMessageBody(1, "\xf0\x9f\x98\x80").empty());
}
