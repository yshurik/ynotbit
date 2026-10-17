#include <bmc/bytes.h>
#include <bmc/test.h>
#include <bmc/text.h>
#include <climits>

namespace {
std::string raw(const char *hexText) {
    return bmc::stringOf(bmc::unhex(hexText));
}
} // namespace

// QString::fromUtf8(bytes).toUtf8() as Qt 6.8.3 answers it; text_parity_test.cpp checks Qt itself.
TEST(sanitize_replaces_every_byte_of_an_invalid_sequence_and_drops_a_leading_bom) {
    struct Case {
        const char *in, *out;
    };
    const Case cases[] = {
        {"efbbbf616263", "616263"}, // a leading byte order mark is dropped
        {"61efbbbf", "61efbbbf"},   // one further on is kept
        {"efbbbf", ""},
        {"", ""},
        {"61ff62", "61efbfbd62"},
        {"61c3", "61efbfbd"},
        {"61c32862", "61efbfbd2862"},
        {"e282", "efbfbdefbfbd"},
        {"e28228", "efbfbdefbfbd28"},
        {"f09f98", "efbfbdefbfbdefbfbd"},
        {"c080", "efbfbdefbfbd"},
        {"c1bf", "efbfbdefbfbd"},
        {"eda080", "efbfbdefbfbdefbfbd"},
        {"f4908080", "efbfbdefbfbdefbfbdefbfbd"},
        {"61808062", "61efbfbdefbfbd62"},
        {"e08080", "efbfbdefbfbdefbfbd"},
        {"f5808080", "efbfbdefbfbdefbfbdefbfbd"},
        {"610062", "610062"}, // NUL is kept
        {"f09f9880", "f09f9880"},
    };
    for (const auto &c : cases)
        CHECK_EQ(bmc::hex(bmc::view(bmc::text::sanitizeUtf8(raw(c.in)))), c.out);
}

TEST(valid_utf8_follows_the_standard) {
    CHECK(bmc::text::validUtf8(""));
    CHECK(bmc::text::validUtf8("plain"));
    CHECK(bmc::text::validUtf8(raw("c3a9e282acf09f9880")));
    CHECK(bmc::text::validUtf8(raw("efbbbf")));    // a byte order mark is valid text
    CHECK(!bmc::text::validUtf8(raw("c0af")));     // overlong
    CHECK(!bmc::text::validUtf8(raw("eda080")));   // a surrogate
    CHECK(!bmc::text::validUtf8(raw("f4908080"))); // above U+10FFFF
    CHECK(!bmc::text::validUtf8(raw("e282")));     // cut short
    CHECK(!bmc::text::validUtf8(raw("80")));
    CHECK_EQ(bmc::text::sequenceLength(raw("c3a9")), size_t(2));
    CHECK_EQ(bmc::text::sequenceLength(raw("c3")), size_t(0));
    CHECK_EQ(bmc::text::sequenceLength(raw("f09f9880")), size_t(4));
    CHECK_EQ(bmc::text::sequenceLength(""), size_t(0));
}

TEST(is_space_is_what_qt_trims) {
    for (char32_t c : {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x20, 0x85, 0xa0, 0x1680, 0x2000, 0x2005,
                       0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000})
        CHECK(bmc::text::isSpace(c));
    for (char32_t c : {0x00,   0x08,   0x0e,   0x1f,   0x21,   0x84,   0x86,    0x9f,
                       0xa1,   0x167f, 0x1681, 0x1fff, 0x200b, 0x2027, 0x202a,  0x202e,
                       0x2030, 0x205e, 0x2060, 0x2fff, 0x3001, 0xfeff, 0x10ffff})
        CHECK(!bmc::text::isSpace(c));
}

TEST(is_blank_means_nothing_left_after_qt_trimmed) {
    CHECK(bmc::text::isBlank(""));
    CHECK(bmc::text::isBlank(" \t\r\n"));
    CHECK(bmc::text::isBlank(
        raw("c2a0e38080e280a8"))); // no-break space, ideographic space, line separator
    CHECK(!bmc::text::isBlank(" a "));
    CHECK(!bmc::text::isBlank(raw("efbbbf"))); // U+FEFF is not white space
    CHECK(!bmc::text::isBlank(raw("20ff20"))); // an invalid byte is not white space
}

TEST(to_int_is_qstring_to_int) {
    struct Case {
        const char *text;
        int value;
        bool ok;
    };
    const Case cases[] = {
        {" 12", 12, true},
        {"12 ", 12, true},
        {"+5", 5, true},
        {"-3", -3, true},
        {"1_0", 0, false},
        {"", 0, false},
        {"99999999999", 0, false},
        {"0x10", 0, false},
        {" \t7", 7, true},
        {"7 x", 0, false},
        {"007", 7, true},
        {"-0", 0, true},
        {"2147483648", 0, false},
        {"-2147483648", INT_MIN, true},
        {"2147483647", INT_MAX, true},
        {"-2147483649", 0, false},
        {"1e3", 0, false},
        {"+ 5", 0, false},
        {"--5", 0, false},
        {"+-5", 0, false},
        {"5-", 0, false},
        {"5+", 0, false},
        {".5", 0, false},
        {"1,000", 0, false},
        {"+", 0, false},
        {"-", 0, false},
        {" ", 0, false},
        {"00", 0, true},
        {"-007", -7, true},
        {"+0", 0, true},
        {"4294967295", 0, false},
    };
    for (const auto &c : cases) {
        bool ok = !c.ok;
        const int value = bmc::text::toInt(c.text, &ok);
        CHECK_EQ(ok, c.ok);
        CHECK_EQ(value, c.value);
    }
    // The white space around a number is ASCII: 9 to 13 and the space.
    for (const char *space : {"\t", "\n", "\v", "\f", "\r", " "})
        CHECK_EQ(bmc::text::toInt(std::string(space) + "7" + space), 7);
    for (const char *notSpace : {"\x1c", "\x1f", "\xc2\x85", "\xc2\xa0", "\xe3\x80\x80"}) {
        bool ok = true;
        bmc::text::toInt(std::string(notSpace) + "7", &ok);
        CHECK(!ok);
    }
}

TEST(truncate_utf8_cuts_between_characters) {
    CHECK_EQ(std::string(bmc::text::truncateUtf8("abc", 10)), "abc");
    CHECK_EQ(std::string(bmc::text::truncateUtf8("", 0)), "");
    CHECK_EQ(std::string(bmc::text::truncateUtf8("a\xc3\xa9", 3)), "a\xc3\xa9");
    CHECK_EQ(std::string(bmc::text::truncateUtf8("a\xc3\xa9", 2)), "a");
    CHECK_EQ(std::string(bmc::text::truncateUtf8("\xf0\x9f\x98\x80\xf0\x9f\x98\x80", 5)),
             "\xf0\x9f\x98\x80");
    CHECK_EQ(std::string(bmc::text::truncateUtf8("\xf0\x9f\x98\x80", 3)), "");
}

TEST(trim_drops_the_white_space_qt_trimmed_and_nothing_else) {
    using bmc::text::trim;
    CHECK_EQ(trim("  a b  "), std::string_view("a b"));
    // QChar::isSpace has multi-byte members: NBSP, U+2000, U+2028, U+2029, U+3000, U+1680, U+0085.
    // The byte offsets of the two loops are where a trim gets this wrong.
    CHECK_EQ(trim("\xc2\xa0\xe2\x80\x80x\xe2\x80\xa9"), std::string_view("x"));
    CHECK_EQ(trim("\xe3\x80\x80x\xe1\x9a\x80"), std::string_view("x"));
    CHECK_EQ(trim(" \xc2\x85x"), std::string_view("x"));
    CHECK_EQ(trim("\xe2\x80\xa8"), std::string_view(""));
    // A four-byte character is not white space: it ends the trim like any other character.
    CHECK_EQ(trim("\xf0\x9f\x98\x80x"), std::string_view("\xf0\x9f\x98\x80x"));
    CHECK_EQ(trim(" \xf0\x9f\x98\x80"), std::string_view("\xf0\x9f\x98\x80"));
    CHECK_EQ(trim("x\xf0\x9f\x98\x80 "), std::string_view("x\xf0\x9f\x98\x80"));
    // A byte that is not part of a well-formed sequence is not white space either: it stops the
    // trim at the front, and at the back everything from it on counts as a character.
    CHECK_EQ(trim("\xff  "), std::string_view("\xff"));
    CHECK_EQ(trim("abc\xff "), std::string_view("abc\xff"));
    // The four-byte lead bytes run from 0xf0 to 0xf4, the last of them only up to U+10FFFF.
    CHECK_EQ(bmc::text::sequenceLength("\xf1\x80\x80\x80"), size_t(4));
    CHECK_EQ(bmc::text::sequenceLength("\xf3\xbf\xbf\xbf"), size_t(4));
    CHECK_EQ(bmc::text::sequenceLength("\xf4\x90\x80\x80"), size_t(0)); // above U+10FFFF
    CHECK(!bmc::text::isBlank("\xf0\x9f\x98\x80")); // four bytes, not white space
    CHECK(bmc::text::isBlank("\xe3\x80\x80"));
    // The payload form: one U+FFFD per invalid byte going in, and '?' for a surrogate that has no
    // partner coming back.
    CHECK(bmc::text::utf16FromUtf8("\xff") == std::u16string(1, u'\ufffd'));
    const auto two_replacement = bmc::text::utf16FromUtf8(std::string("a\xff\xff"
                                                                      "b"));
    const std::u16string two_expected{u'a', char16_t(0xfffd), char16_t(0xfffd), u'b'};
    CHECK(two_replacement == two_expected);
    CHECK(bmc::text::utf8FromUtf16(std::u16string{char16_t(0xdc00)}) == std::string("?"));
}
