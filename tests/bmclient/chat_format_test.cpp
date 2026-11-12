#include <bmc/chat_format.h>
#include <bmc/error.h>
#include <bmc/identity.h>
#include <bmc/test.h>
#include <cstdint>
#include <fstream>

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
}

// A chat has no limit of its own. It is a message with no subject whose text is the JSON of
// chatMessageBody(), so it may be as long as a letter's text, which is what an object (2^18 bytes)
// holds less its header, keys, acknowledgment, signature and encryption. chatFits() counts the
// bytes that go on the wire, the sequence number at its longest.
namespace {
size_t largestPlainBody() {
    return size_t(bmc::kMaxLetterText) -
           size_t(bmc::letterTextBytes("", bmc::chatMessageBody(INT64_MAX, "")));
}
} // namespace

TEST(a_chat_fits_when_its_message_text_fits_a_letters) {
    const auto most = largestPlainBody();
    // The Go bot (btcbot) holds its replies to the same number, from the same file.
    std::ifstream in(std::string(BMC_NICKBOT_TESTDATA) + "/chat_limit.txt");
    CHECK(in.good());
    std::string line;
    size_t fromFile = 0;
    while (std::getline(in, line))
        if (!line.empty() && line[0] != '#')
            fromFile = size_t(std::stoul(line));
    CHECK_EQ(most, fromFile);
    CHECK(most > 260000); // nothing like 8192
    CHECK(bmc::chatFits("hi"));
    CHECK(bmc::chatFits(std::string(most, 'a')));
    CHECK(!bmc::chatFits(std::string(most + 1, 'a')));
}

TEST(what_json_writes_in_more_bytes_counts_for_them) {
    const auto most = largestPlainBody();
    CHECK(bmc::chatFits(std::string(most / 2, '"'))); // a quote is two bytes
    CHECK(!bmc::chatFits(std::string(most / 2 + 1, '"')));
    CHECK(bmc::chatFits(std::string(most / 2, '\\')));
    CHECK(!bmc::chatFits(std::string(most / 2 + 1, '\\')));
    CHECK(bmc::chatFits(std::string(most / 6, '\x01'))); // another control character is six
    CHECK(!bmc::chatFits(std::string(most / 6 + 1, '\x01')));
    CHECK(bmc::chatFits(std::string(most / 2, '\n')));
    CHECK(!bmc::chatFits(std::string(most / 2 + 1, '\n')));
    std::string accents; // two bytes in UTF-8 and written as they are
    for (size_t i = 0; i < most / 2; ++i)
        accents += "\xc3\xa9";
    CHECK(bmc::chatFits(accents));
    CHECK(!bmc::chatFits(accents + "\xc3\xa9"));
}

TEST(a_chat_that_is_no_utf_8_is_an_error_not_a_size) {
    CHECK_THROWS(bmc::chatFits("bad \xff byte"));
}

TEST(a_received_chat_of_any_size_an_object_can_carry_is_a_chat) {
    bmc::ChatPayload payload;
    const std::string big(255000, 'x');
    CHECK(parse(bmc::chatMessageBody(1, big), &payload) == ChatParseResult::Chat);
    CHECK_EQ(payload.body.size(), size_t(255000));
    // Another client's chat may be a little longer than ours may send: its sequence number is
    // shorter.
    const std::string other(largestPlainBody() + 5, 'x');
    CHECK(parse(bmc::chatMessageBody(1, other), &payload) == ChatParseResult::Chat);
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
    // A chat has no size of its own to hold to: what an object carried is read, whatever its length
    // (it was refused over 8192 bytes, until 2026-11-12).
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"" + std::string(8193, 'x') + "\"")) ==
          ChatParseResult::Chat);
    std::string accents;
    for (int i = 0; i < 4097; ++i)
        accents += "\xc3\xa9";
    CHECK(parse(chat("1", "\"markdown\"", "1", "\"" + accents + "\"")) == ChatParseResult::Chat);
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
