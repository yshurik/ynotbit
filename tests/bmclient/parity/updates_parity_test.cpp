#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <bmc/bytes.h>
#include <bmc/test.h>
#include <bmc/updates.h>
#include <optional>
#include <random>

namespace {
// src/updates.cpp as it was before slice 1.
std::optional<QString> oldAnnounced(const QString &subject) {
    static const QRegularExpression version("^\\s*ynotbit\\s+v?(\\d{1,4}(?:\\.\\d{1,4}){1,3})\\s*$",
                                            QRegularExpression::CaseInsensitiveOption);
    const auto v = version.match(subject);
    if (!v.hasMatch())
        return {};
    return v.captured(1);
}
int oldCompare(const QString &a, const QString &b) {
    const auto x = a.split('.'), y = b.split('.');
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i].toInt() : 0, q = i < y.size() ? y[i].toInt() : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}
QString q(const std::string &s) {
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}
std::string join(std::mt19937_64 &rng, const std::vector<std::string> &pieces, int most) {
    std::string out;
    for (int i = 1 + int(rng() % most); i > 0; --i)
        out += pieces[rng() % pieces.size()];
    return out;
}
} // namespace

TEST(announced_version_matches_the_old_regular_expression) {
    std::mt19937_64 rng(0x5eed0007);
    const std::vector<std::string> pieces = {
        "ynotbit", "YnotBit", "ynotb1t", "v",        "V", " ", " ",           "\t", "\n",
        "\v",      "\f",      "\r",      "\xc2\xa0", "0", "1", "5",           "7",  "12",
        "1234",    "12345",   ".",       ".",        "x", "-", "\xe2\x80\xaf"};
    for (int round = 0; round < 300000; ++round) {
        std::string subject = (round % 2 ? "ynotbit " : "") + join(rng, pieces, 9);
        const auto expected = oldAnnounced(q(subject));
        const auto actual = bmc::updates::announcedVersion(subject);
        const bool equal = expected.has_value() == actual.has_value() &&
                           (!actual || expected->toUtf8().toStdString() == *actual);
        if (!equal)
            bmc::test::fail(__FILE__, __LINE__,
                            "announcedVersion differs for " + bmc::hex(bmc::view(subject)));
    }
}

TEST(compare_versions_matches_the_old_one) {
    std::mt19937_64 rng(0x5eed0008);
    const std::vector<std::string> pieces = {"0",   "1", "2",          "9",  "10",      "99",
                                             "007", ".", ".",          "..", " ",       "x",
                                             "+",   "-", "2147483648", "\t", "\xc2\xa0"};
    for (int round = 0; round < 300000; ++round) {
        const auto a = join(rng, pieces, 8), b = join(rng, pieces, 8);
        const int expected = oldCompare(q(a), q(b)), actual = bmc::updates::compareVersions(a, b);
        if (expected != actual)
            bmc::test::fail(__FILE__, __LINE__,
                            "compareVersions differs for " + bmc::hex(bmc::view(a)) + " / " +
                                bmc::hex(bmc::view(b)));
    }
}
