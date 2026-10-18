#include <bmc/json.h>
#include <bmc/test.h>
#include <clocale>
#include <cstdint>
#include <string>

using bmc::json::parse;
using bmc::json::Value;

namespace {
std::string str(const char *s) {
    return s;
}
} // namespace

TEST(json_reads_the_forms_qt_read) {
    const auto doc = parse(R"({"a":1,"b":[true,null,"x"],"c":{"d":-2.5e1}})");
    CHECK(doc && doc->isObject());
    CHECK_EQ(doc->at("a").toInteger(), 1);
    CHECK(doc->at("a").equalsNumber(1));
    CHECK_EQ(doc->at("b").items().size(), size_t(3));
    CHECK(doc->at("b").items()[0].toBool());
    CHECK(doc->at("b").items()[1].isNull());
    CHECK_EQ(doc->at("b").items()[2].toString(), "x");
    CHECK_EQ(doc->at("c").at("d").toDouble(), -25.0);
    CHECK(doc->at("missing").isUndefined());
    CHECK(doc->at("a").at("x").isUndefined()); // a number has no members
    CHECK_EQ(doc->keys().size(), size_t(3));
}

TEST(json_numbers_follow_qt) {
    const auto n = [](const char *text) {
        return parse(std::string(R"({"a":)") + text + "}")->at("a");
    };
    CHECK(n("1.0").equalsNumber(1) && n("1e0").equalsNumber(1));
    CHECK(!n("\"1\"").equalsNumber(1));
    CHECK_EQ(n("3.0").toInteger(), 3);
    CHECK_EQ(n("1e2").toInteger(), 100);
    CHECK_EQ(n("1.5e3").toInteger(), 1500);
    CHECK_EQ(n("2.5").toInteger(7), 7);
    CHECK_EQ(n("9223372036854775807").toInteger(), INT64_MAX);
    CHECK_EQ(n("-9223372036854775808").toInteger(), INT64_MIN);
    CHECK_EQ(n("9223372036854775808").toInteger(7), 7);
    CHECK_EQ(n("123456789012345678901234567890").toInteger(7), 7);
    CHECK_EQ(n(".5").toDouble(), 0.5);
    CHECK_EQ(n("5.").toInteger(), 5);
    CHECK_EQ(n("-0").toInteger(), 0);
    CHECK_EQ(n("1E+2").toInteger(), 100);
    CHECK_EQ(n("0.1").toDouble(), 0.1);
    CHECK_EQ(n("\"x\"").toInteger(7), 7); // not a number
}

TEST(json_strings_follow_qt) {
    const auto s = [](const std::string &literal) {
        const auto doc = parse("{\"a\":" + literal + "}");
        return doc ? doc->at("a").toString() : std::string("<error>");
    };
    CHECK_EQ(s(R"("\/\b\f\n\r\t\"\\")"), "/\b\f\n\r\t\"\\");
    CHECK_EQ(s(R"("\u00e9")"), "\xc3\xa9");
    CHECK_EQ(s(R"("\ud83d\ude00")"), "\xf0\x9f\x98\x80");
    CHECK_EQ(s(R"("\ud83d")"), "?");
    CHECK_EQ(s(R"("\ude00")"), "?");
    CHECK_EQ(s(R"("\ud83d\u0041")"), "?A");
    CHECK_EQ(s(R"("\ude00\ud83d")"), "??");
    CHECK_EQ(s(R"("\u0000")"), std::string(1, '\0'));
    CHECK_EQ(s("\"x\ny\""), "x\ny");     // a raw line feed
    CHECK_EQ(s("\"x\ty\""), "x\ty");     // a raw tab
    CHECK_EQ(s("\"x\x01y\""), "x\x01y"); // a raw control character
    CHECK_EQ(s(R"("\x")"), "x");         // an unknown escape stands for itself
    CHECK_EQ(s("\"\xc3\xa9 \xf0\x9f\x98\x80\""), "\xc3\xa9 \xf0\x9f\x98\x80");
}

TEST(json_rejects_what_qt_rejected) {
    for (const char *bad : {"1",
                            "",
                            "{",
                            "}",
                            "[",
                            "{\"a\":1} x",
                            "{\"a\":1}{\"a\":2}",
                            "{\"a\":01}",
                            "{'a':1}",
                            "{\"a\":+1}",
                            "{\"a\":1e}",
                            "{\"a\":0x1}",
                            "{\"a\":-}",
                            "{\"a\":1e999}",
                            "{\"a\":-1e999}",
                            "{\"a\":Infinity}",
                            "{\"a\":NaN}",
                            "{\"a\":True}",
                            "{\"a\":nul}",
                            "{\"a\":1,}",
                            "{\"a\" 1}",
                            "{\"a\":1 //c\n}",
                            "{\"a\":1}\f",
                            "\f{\"a\":1}",
                            "\v{\"a\":1}",
                            "{\"a\":[1,2,]}",
                            "{a:1}",
                            "{\"a\":\"\\u12\"}",
                            "{\"a\":\"\\u12G4\"}",
                            "{\"a\":\"\xc3\x28\"}",
                            "{\"a\":.e1}",
                            "{\"a\":\"x}",
                            "{\"a\":\"\\",
                            "{\"a\":1e-999}",
                            "{\"a\":2.4e-324}",
                            "{\"a\":1.8e308}"})
        CHECK(!parse(bad));
    std::string deep = "{\"a\":";
    for (int i = 0; i < 1100; ++i)
        deep += "[";
    deep += "1";
    for (int i = 0; i < 1100; ++i)
        deep += "]";
    CHECK(!parse(deep + "}"));
}

TEST(json_accepts_the_rest) {
    CHECK(parse("[1]") && parse("[1]")->isArray());
    CHECK(parse("  {\"a\":null}  "));
    CHECK(parse("\t\r\n {\"a\":1}\n"));
    CHECK(parse("\xef\xbb\xbf{\"a\":1}")); // a leading byte order mark
    CHECK(parse("{\"\":1}") && parse("{\"\":1}")->at("").toInteger() == 1);
    CHECK_EQ(parse("{\"a\":1,\"a\":2}")->at("a").toInteger(), 2); // the last duplicate wins
    CHECK(parse("{\"a\":{}}")->at("a").isObject());
    CHECK(parse("{\"a\":[]}")->at("a").isArray());
    CHECK(parse("{\"a\":0e999}") && parse("{\"a\":4.9e-324}") && parse("{\"a\":1e-320}") &&
          parse("{\"a\":1e308}"));
    std::string ok = "{\"a\":";
    for (int i = 0; i < 1000; ++i)
        ok += "[";
    ok += "1";
    for (int i = 0; i < 1000; ++i)
        ok += "]";
    CHECK(parse(ok + "}"));
}

TEST(json_quote_escapes_what_qt_escaped) {
    CHECK_EQ(bmc::json::quote("a\"b\\c/"), R"("a\"b\\c/")");
    CHECK_EQ(bmc::json::quote(std::string("\b\t\n\f\r\x01\x1f\x0b", 8)),
             R"("\b\t\n\f\r\u0001\u001f\u000b")");
    CHECK_EQ(bmc::json::quote(std::string("\0", 1)), R"("\u0000")");
    // Everything else stays as it is: DEL, C1 controls, no-break space, line separator, an emoji.
    CHECK_EQ(bmc::json::quote("\x7f\xc2\x80\xc2\xa0\xe2\x80\xa8\xf0\x9f\x98\x80"),
             "\"\x7f\xc2\x80\xc2\xa0\xe2\x80\xa8\xf0\x9f\x98\x80\"");
    // quote() output parses back to the same text.
    const std::string text = "mixed \"quotes\" \\ and \n lines \xc3\xa9 \xf0\x9f\x98\x80";
    CHECK_EQ(parse("{\"a\":" + bmc::json::quote(text) + "}")->at("a").toString(), text);
}

TEST(json_reads_the_false_literal_and_the_escapes_left_over) {
    // The false literal was the one Qt form the suite had never read.
    CHECK(!parse("{\"a\":false}")->at("a").toBool(true));
    CHECK(parse("{\"a\":true}")->at("a").toBool(false));
    // toDouble and toInteger of something that is not a number are the fallback asked for, not zero
    // and not a throw.
    CHECK(parse("{\"a\":false}")->at("a").toDouble(1.5) == 1.5);
    CHECK(parse("{\"a\":\"1\"}")->at("a").toInteger(7) == 7);
    CHECK(parse("{\"a\":1.5}")->at("a").toDouble() == 1.5);
    // A \u escape with upper-case hex digits, one that is a surrogate pair, and one cut short.
    CHECK_EQ(parse("[\"\\u00E9\"]")->items()[0].toString(), std::string("\xc3\xa9"));
    CHECK_EQ(parse("[\"\\uD83D\\uDE00\"]")->items()[0].toString(), std::string("\xf0\x9f\x98\x80"));
    CHECK(!parse("[\"\\u00\"]"));
    // A backslash before a byte of 0x80 or more: Qt read the byte as the Latin-1 character of that
    // number.
    CHECK_EQ(parse("[\"\\\xe9\"]")->items()[0].toString(), std::string("\xc3\xa9"));
    // A document that ends in a number is not one (Qt's parser wanted something after it), and
    // neither is one nested deeper than its limit of object levels.
    CHECK(!parse("[1"));
    std::string deep;
    for (int i = 0; i < 1100; ++i)
        deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < 1100; ++i)
        deep += "}";
    CHECK(!parse(deep));
    // A decimal point that is not the C locale's is still read as one: a peer's number does not
    // depend on the machine that reads it.
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        CHECK(parse("[1.5]")->items()[0].toDouble() == 1.5);
        std::setlocale(LC_NUMERIC, "C");
    }
}
