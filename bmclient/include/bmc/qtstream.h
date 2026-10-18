// The QDataStream subset the vault payload uses, and nothing else. A quint32 is big-endian; a
// QString is a quint32 byte length then UTF-16BE, and a null string is 0xFFFFFFFF (read as empty);
// a bool is one byte; raw bytes are as given. Confirmed against Qt 6.8.3 by a probe program and
// frozen in tests/golden/qtstream.txt.
#pragma once
#include <bmc/bytes.h>
#include <cstdint>
#include <string>

namespace bmc {
class qtstream {
    Bytes bytes_;
    size_t at_ = 0;
    bool ok_ = true;

  public:
    qtstream() = default;
    explicit qtstream(Bytes in) : bytes_(std::move(in)) {}

    qtstream &operator<<(uint32_t v);
    qtstream &operator<<(bool v);
    qtstream &operator<<(const std::string &v);
    void writeRaw(ByteView raw);

    qtstream &operator>>(uint32_t &v);
    qtstream &operator>>(bool &v);
    qtstream &operator>>(std::string &v);
    // Reads n raw bytes. Fewer than n sets ok() false and returns empty.
    Bytes readRaw(size_t n);

    const Bytes &bytes() const {
        return bytes_;
    }
    bool ok() const {
        return ok_;
    }
    bool atEnd() const {
        return at_ == bytes_.size();
    }
};
} // namespace bmc
