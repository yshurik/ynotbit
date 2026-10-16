// Bytes, and the few things every other file does with them.
#pragma once
#include <bmc/error.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bmc {
using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;

inline ByteView view(std::string_view s) {
    return {reinterpret_cast<const uint8_t *>(s.data()), s.size()};
}
inline std::string_view chars(ByteView b) {
    return {reinterpret_cast<const char *>(b.data()), b.size()};
}
inline Bytes bytesOf(std::string_view s) {
    const auto v = view(s);
    return Bytes(v.begin(), v.end());
}
inline Bytes bytesOf(ByteView b) {
    return Bytes(b.begin(), b.end());
}
inline std::string stringOf(ByteView b) {
    return std::string(chars(b));
}
inline void append(Bytes &out, ByteView more) {
    out.insert(out.end(), more.begin(), more.end());
}
Bytes concat(std::initializer_list<ByteView> parts);
// Overwrites with zeros, then empties.
void wipe(Bytes &b);
bool equalConstantTime(ByteView a, ByteView b);
// Lower-case hexadecimal.
std::string hex(ByteView b);
// Throws Error(Argument) on an odd length or a character that is no hexadecimal digit.
Bytes unhex(std::string_view text);
// Big-endian integers of 1 to 8 bytes.
void putBE(Bytes &out, uint64_t value, int bytes);
uint64_t getBE(ByteView in);
// Bitmessage's variable-length integer.
void putVarInt(Bytes &out, uint64_t value);
} // namespace bmc
