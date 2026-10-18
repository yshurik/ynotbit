// The core's qtstream against a live QDataStream at Qt_6_0, over a grid of text, numbers and raw
// bytes. tests/golden/qtstream.txt freezes what Qt 6.8.3 wrote; this test is what notices when Qt
// itself changes the encoding under the vault's feet.
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>
#include <bmc/bytes.h>
#include <bmc/qtstream.h>
#include <bmc/test.h>

namespace {
QByteArray qtBytes(const QString &text) {
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    s << text;
    return bytes;
}
std::string hexOf(const QByteArray &bytes) {
    return std::string(bytes.toHex().constData());
}
bmc::Bytes bytesOfQ(const QByteArray &bytes) {
    const auto *data = reinterpret_cast<const uint8_t *>(bytes.constData());
    return bmc::Bytes(data, data + bytes.size());
}
} // namespace

TEST(qtstreams_text_bytes_are_qdatastreams) {
    const std::string texts[] = {
        "A",
        "Bitmessage",
        "\xe2\x82\xac",                         // U+20AC, one BMP unit
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", // U+65E5 U+672C U+8A9E, three BMP units
        "\xf0\x9f\x98\x80",                     // U+1F600, a surrogate pair
        "A\xf0\x9f\x98\x80\xc3\xa9",            // mixed
        std::string(255, 'x'),                  // 255 units, 510 payload bytes
        "",                                     // the empty string is not the null one
    };
    for (const auto &text : texts) {
        const QString q = QString::fromUtf8(text.data(), qsizetype(text.size()));
        CHECK(!q.isNull()); // a null QString writes ffffffff and has no core twin
        bmc::qtstream out;
        out << text;
        CHECK_EQ(bmc::hex(out.bytes()), hexOf(qtBytes(q)));
        // And reading Qt's bytes back gives the same UTF-8.
        std::string back;
        bmc::qtstream in(bytesOfQ(qtBytes(q)));
        in >> back;
        CHECK(in.ok() && in.atEnd());
        CHECK_EQ(back, text);
    }
    // A null QString is the one case the core cannot write: 0xFFFFFFFF, read as empty.
    CHECK_EQ(hexOf(qtBytes(QString())), std::string("ffffffff"));
    std::string empty = "x";
    bmc::qtstream null(bmc::unhex("ffffffff"));
    null >> empty;
    CHECK(null.ok() && empty.empty());
}

TEST(qtstreams_numbers_and_raw_bytes_are_qdatastreams) {
    for (uint32_t value : {0u, 1u, 255u, 65535u, 4294967295u, 1234567890u}) {
        QByteArray expected;
        QDataStream s(&expected, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << quint32(value);
        bmc::qtstream out;
        out << value;
        CHECK_EQ(bmc::hex(out.bytes()), hexOf(expected));
    }
    for (bool value : {false, true}) {
        QByteArray expected;
        QDataStream s(&expected, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s << value;
        bmc::qtstream out;
        out << value;
        CHECK_EQ(bmc::hex(out.bytes()), hexOf(expected));
    }
    for (size_t length : {size_t(0), size_t(4), size_t(255)}) {
        bmc::Bytes raw;
        for (size_t i = 0; i < length; ++i)
            raw.push_back(uint8_t(i));
        bmc::qtstream out;
        out.writeRaw(raw);
        CHECK_EQ(bmc::hex(out.bytes()), bmc::hex(raw)); // raw bytes go through verbatim
    }
}
