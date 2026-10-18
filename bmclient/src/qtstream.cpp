#include <bmc/qtstream.h>
#include <bmc/text.h>

namespace bmc {
namespace {
void put32(Bytes &b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
bool get32(ByteView b, size_t &at, uint32_t &v) {
    if (at + 4 > b.size())
        return false;
    v = uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
    at += 4;
    return true;
}
} // namespace

qtstream &qtstream::operator<<(uint32_t v) {
    put32(bytes_, v);
    return *this;
}

qtstream &qtstream::operator<<(bool v) {
    bytes_.push_back(v ? 1 : 0);
    return *this;
}

qtstream &qtstream::operator<<(const std::string &v) {
    const auto units = text::utf16FromUtf8(v);
    put32(bytes_, uint32_t(units.size() * 2));
    for (char16_t c : units) {
        bytes_.push_back(uint8_t(c >> 8));
        bytes_.push_back(uint8_t(c));
    }
    return *this;
}

void qtstream::writeRaw(ByteView raw) {
    bytes_.insert(bytes_.end(), raw.begin(), raw.end());
}

Bytes qtstream::readRaw(size_t n) {
    if (at_ + n > bytes_.size()) {
        ok_ = false;
        return {};
    }
    Bytes out(bytes_.begin() + at_, bytes_.begin() + at_ + n);
    at_ += n;
    return out;
}

qtstream &qtstream::operator>>(uint32_t &v) {
    if (!get32(bytes_, at_, v))
        ok_ = false;
    return *this;
}

qtstream &qtstream::operator>>(bool &v) {
    if (at_ + 1 > bytes_.size()) {
        ok_ = false;
        return *this;
    }
    v = bytes_[at_++] != 0;
    return *this;
}

qtstream &qtstream::operator>>(std::string &v) {
    uint32_t n = 0;
    if (!get32(bytes_, at_, n)) {
        ok_ = false;
        return *this;
    }
    if (n == 0xffffffffu) { // a null QString reads as empty
        v.clear();
        return *this;
    }
    if (n % 2 != 0 || at_ + n > bytes_.size()) {
        ok_ = false;
        return *this;
    }
    std::u16string units;
    units.reserve(n / 2);
    for (uint32_t i = 0; i < n; i += 2)
        units.push_back(char16_t(uint16_t(bytes_[at_ + i]) << 8 | bytes_[at_ + i + 1]));
    at_ += n;
    v = text::utf8FromUtf16(units);
    return *this;
}
} // namespace bmc
