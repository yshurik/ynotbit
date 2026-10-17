#include <bmc/qtstream.h>
#include <bmc/test.h>

using bmc::qtstream;

TEST(qtstream_writes_the_bytes_qt_wrote) {
    // The bytes a QDataStream at Qt_6_0 writes (confirmed by a Qt 6.8.3 probe in this task).
    qtstream out;
    out << uint32_t(0x01020304) << false << true;
    out << std::string("A") << std::string("\xe2\x82\xac") << std::string("\xf0\x9f\x98\x80")
        << std::string();
    out.writeRaw(bmc::unhex("00ff"));
    CHECK_EQ(bmc::hex(out.bytes()), std::string("01020304"
                                                "00"
                                                "01"
                                                "000000020041"
                                                "0000000220ac"
                                                "00000004d83dde00"
                                                "00000000"
                                                "00ff"));
}

TEST(qtstream_reads_the_bytes_qt_wrote) {
    qtstream in(bmc::unhex("01020304"
                           "00"
                           "01"
                           "000000020041"
                           "ffffffff"));
    uint32_t n = 0;
    bool f = true, t = false;
    std::string a, empty = "x";
    in >> n >> f >> t >> a >> empty;
    CHECK_EQ(n, uint32_t(0x01020304));
    CHECK(!f && t);
    CHECK_EQ(a, std::string("A"));
    CHECK_EQ(empty, std::string()); // a null QString reads as empty
    CHECK(in.ok() && in.atEnd());
}

TEST(qtstream_round_trips_non_bmp_text_and_a_lone_surrogate) {
    qtstream out;
    out << std::string("\xf0\x9f\x98\x80"); // U+1F600
    std::string back;
    qtstream(out.bytes()) >> back;
    CHECK_EQ(back, std::string("\xf0\x9f\x98\x80"));
    // A lone high surrogate in the payload becomes '?', as QString::toUtf8 makes it.
    qtstream lone(bmc::unhex("00000002d83d"));
    std::string s;
    lone >> s;
    CHECK_EQ(s, std::string("?"));
}

TEST(qtstream_refuses_a_truncated_or_oversized_payload) {
    {
        qtstream in(bmc::unhex("000000"));
        uint32_t v = 0;
        in >> v;
        CHECK(!in.ok()); // a quint32 cut short
    }
    {
        qtstream in(bmc::unhex("0000000600"));
        std::string s;
        in >> s;
        CHECK(!in.ok()); // a string longer than what is there
    }
    {
        qtstream in(bmc::unhex("00000001ff"));
        std::string s;
        in >> s;
        CHECK(!in.ok()); // an odd string length
    }
    {
        qtstream in(bmc::unhex("000000000041"));
        std::string s;
        in >> s;
        CHECK(in.ok());
        CHECK(!in.atEnd()); // one byte left over
    }
}
