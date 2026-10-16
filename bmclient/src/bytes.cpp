#include <bmc/bytes.h>
#include <sodium.h>

namespace bmc {
Bytes concat(std::initializer_list<ByteView> parts) {
    size_t total = 0;
    for (auto part : parts)
        total += part.size();
    Bytes out;
    out.reserve(total);
    for (auto part : parts)
        append(out, part);
    return out;
}

void wipe(Bytes &b) {
    if (!b.empty())
        sodium_memzero(b.data(), b.size());
    b.clear();
}

bool equalConstantTime(ByteView a, ByteView b) {
    return a.size() == b.size() && (a.empty() || sodium_memcmp(a.data(), b.data(), a.size()) == 0);
}

std::string hex(ByteView b) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (auto c : b) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

Bytes unhex(std::string_view text) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if (text.size() % 2)
        throw Error(Code::Argument, "Invalid hexadecimal text");
    Bytes out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]), low = nibble(text[i + 1]);
        if (high < 0 || low < 0)
            throw Error(Code::Argument, "Invalid hexadecimal text");
        out.push_back(uint8_t(high << 4 | low));
    }
    return out;
}

void putBE(Bytes &out, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(uint8_t(value >> (i * 8)));
}

uint64_t getBE(ByteView in) {
    uint64_t value = 0;
    for (auto c : in)
        value = (value << 8) | c;
    return value;
}

void putVarInt(Bytes &out, uint64_t value) {
    if (value < 253) {
        out.push_back(uint8_t(value));
        return;
    }
    const int size = value <= 0xffff ? 2 : value <= 0xffffffffULL ? 4 : 8;
    out.push_back(size == 2 ? 253 : size == 4 ? 254 : 255);
    putBE(out, value, size);
}
} // namespace bmc
