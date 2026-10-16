#include <bmc/test.h>
#include <bmc/updates.h>

using namespace bmc::updates;

TEST(announced_version_reads_a_release_subject) {
    struct Case {
        const char *subject;
        const char *version; // nullptr: not an announcement
    };
    const Case cases[] = {
        {"ynotbit 0.5.2", "0.5.2"},
        {"ynotbit v1.2", "1.2"},
        {"YNOTBIT V1.2", "1.2"},
        {"  ynotbit\t0.7.1  ", "0.7.1"},
        {"ynotbit 0.7.1\n", "0.7.1"},
        {"ynotbit  v0.5", "0.5"},
        {"ynotbit 0.7.1.2", "0.7.1.2"},
        {"ynotbit 1234.5678", "1234.5678"},
        {"ynotbit 0.7.1.2.3", nullptr},
        {"ynotbit 1", nullptr},
        {"ynotbit 12345.1", nullptr},
        {"ynotbit0.5", nullptr},
        {"ynotbit 0.5 beta", nullptr},
        {"Re: ynotbit 0.6.0", nullptr},
        {"ynotbit 0.6.0 is out, get it at evil.example", nullptr},
        {"ynotbit v", nullptr},
        {"ynotbit 0.", nullptr},
        {"ynotbit .5", nullptr},
        {"ynotbit 0..5", nullptr},
        {"", nullptr},
        {"ynotbit 0.5\xc2\xa0", nullptr},
        {"ynotbit\xc2\xa0"
         "0.5",
         nullptr},
    };
    for (const auto &c : cases) {
        const auto v = announcedVersion(c.subject);
        if (c.version) {
            CHECK(v.has_value());
            CHECK_EQ(*v, c.version);
        } else {
            CHECK(!v.has_value());
        }
    }
}

TEST(compare_versions_is_numeric_per_part) {
    CHECK(compareVersions("0.10.0", "0.9.9") > 0);
    CHECK(compareVersions("0.9", "0.10") < 0);
    CHECK_EQ(compareVersions("0.5", "0.5.0"), 0);
    CHECK(compareVersions("0.5.1", "0.6") < 0);
    CHECK_EQ(compareVersions("1.0", "1"), 0);
    CHECK_EQ(compareVersions("", ""), 0);
    CHECK_EQ(compareVersions("1..2", "1.0.2"), 0); // an empty part is 0
    CHECK_EQ(compareVersions("1.x", "1.0"), 0);    // so is a part that is no number
    CHECK_EQ(compareVersions(" 7", "7"), 0);       // QString::toInt allowed spaces around a number
    CHECK(compareVersions("1.2.3", "1.2.4") < 0);
    CHECK(compareVersions("2", "1.9.9") > 0);
}

TEST(release_url_and_the_fixed_addresses) {
    CHECK_EQ(releaseUrl("0.7.1"), "https://github.com/yshurik/ynotbit/releases/tag/v0.7.1");
    CHECK_EQ(publisherAddress(), "BM-2666hf5eAbjCJMaPwC7eG3Um55QJGM");
    CHECK_EQ(digestAddress(), "BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9");
    setPublisherAddressForTesting("BM-test");
    CHECK_EQ(publisherAddress(), "BM-test");
    setPublisherAddressForTesting("BM-2666hf5eAbjCJMaPwC7eG3Um55QJGM");
}
