#include <QChar>
#include <QString>
#include <bmc/bytes.h>
#include <bmc/test.h>
#include <bmc/text.h>
#include <iterator>
#include <random>

namespace {
std::string hexOf(const std::string &s) {
    return bmc::hex(bmc::view(s));
}
std::string qtSanitize(const std::string &in) {
    return QString::fromUtf8(in.data(), qsizetype(in.size())).toUtf8().toStdString();
}
} // namespace

TEST(sanitize_utf8_matches_qt_on_random_bytes) {
    std::mt19937_64 rng(0x5eed0001);
    const unsigned char alphabet[] = {0x00, 0x20, 0x41, 0x7f, 0x80, 0x8f, 0x90, 0x9f,
                                      0xa0, 0xbf, 0xc0, 0xc2, 0xdf, 0xe0, 0xe2, 0xed,
                                      0xef, 0xbb, 0xf0, 0xf4, 0xf5, 0xff};
    for (int round = 0; round < 200000; ++round) {
        std::string in;
        const int length = int(rng() % 9);
        for (int i = 0; i < length; ++i)
            in.push_back(char(alphabet[rng() % sizeof alphabet]));
        if (rng() % 8 == 0)
            in = "\xEF\xBB\xBF" + in; // now and then, a byte order mark first
        const auto expected = qtSanitize(in);
        const auto actual = bmc::text::sanitizeUtf8(in);
        if (actual != expected)
            bmc::test::fail(__FILE__, __LINE__,
                            "sanitizeUtf8 differs for " + hexOf(in) + ": " + hexOf(actual) +
                                ", Qt " + hexOf(expected));
        // Without a leading mark, the text is valid exactly when Qt gives it back unchanged.
        if (in.compare(0, 3, "\xEF\xBB\xBF") != 0 && bmc::text::validUtf8(in) != (expected == in))
            bmc::test::fail(__FILE__, __LINE__, "validUtf8 differs for " + hexOf(in));
    }
}

TEST(is_space_matches_qt_for_every_code_point) {
    for (char32_t c = 0; c <= 0x10ffff; ++c)
        if (bmc::text::isSpace(c) != QChar::isSpace(c))
            bmc::test::fail(__FILE__, __LINE__,
                            "isSpace differs at U+" + std::to_string(unsigned(c)));
}

TEST(is_blank_matches_qt_trimmed) {
    std::mt19937_64 rng(0x5eed0002);
    const char32_t pool[] = {0x20,   0x09,   0x0a,   0xa0, 0x85, 0x1680,  0x2003,
                             0x2028, 0x3000, 0xfeff, 0x41, 0xe9, 0x1f600, 0x200b};
    for (int round = 0; round < 50000; ++round) {
        QString text;
        const int length = int(rng() % 6);
        for (int i = 0; i < length; ++i)
            text += QString::fromUcs4(&pool[rng() % std::size(pool)], 1);
        CHECK_EQ(bmc::text::isBlank(text.toUtf8().toStdString()), text.trimmed().isEmpty());
    }
}

TEST(to_int_matches_qt_on_random_text) {
    std::mt19937_64 rng(0x5eed0003);
    const char alphabet[] = "0123456789+- \t\n\v\f\r.xe,_\x1f";
    for (int round = 0; round < 300000; ++round) {
        std::string in;
        const bool digitsOnly = round % 3 == 0;
        const int length = int(rng() % (digitsOnly ? 13 : 9));
        for (int i = 0; i < length; ++i)
            in.push_back(digitsOnly ? char('0' + rng() % 10)
                                    : alphabet[rng() % (sizeof alphabet - 1)]);
        if (digitsOnly && rng() % 2)
            in = "-" + in;
        if (rng() % 16 == 0)
            in += "\xc2\xa0";
        bool qtOk = false;
        const int qt = QString::fromUtf8(in.data(), qsizetype(in.size())).toInt(&qtOk);
        bool ok = !qtOk;
        const int value = bmc::text::toInt(in, &ok);
        if (ok != qtOk || value != qt)
            bmc::test::fail(__FILE__, __LINE__,
                            "toInt differs for " + hexOf(in) + ": " + std::to_string(value) + "/" +
                                std::to_string(ok) + ", Qt " + std::to_string(qt) + "/" +
                                std::to_string(qtOk));
    }
}
