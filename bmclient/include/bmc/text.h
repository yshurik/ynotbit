// UTF-8 text the way the Qt app saw it. QString decoded UTF-8 with its own rules, trimmed with its
// own idea of white space and counted in UTF-16; the answers below are those of Qt 6.8, so that
// what a received message says does not change by moving the code out of Qt.
#pragma once
#include <string>
#include <string_view>

namespace bmc::text {
// The length (1 to 4) of the well-formed UTF-8 sequence at the start of s; 0 if there is none (a
// stray continuation byte, an overlong form, a surrogate, a value above U+10FFFF, a cut-short one).
size_t sequenceLength(std::string_view s);
// What QString::fromUtf8(bytes).toUtf8() returns: a leading byte order mark is dropped, and every
// byte of an invalid sequence is replaced by U+FFFD (one replacement per byte, not per sequence).
std::string sanitizeUtf8(std::string_view bytes);
bool validUtf8(std::string_view bytes);
// QChar::isSpace(): the white space QString::trimmed() removes.
bool isSpace(char32_t c);
// QString::trimmed().isEmpty(), for text that is valid UTF-8 (a byte that is not is not white
// space).
bool isBlank(std::string_view utf8);
// QString::toInt(&ok) in base 10: ASCII white space (9 to 13, the space) around, one sign,
// digits, 32 bits; otherwise 0 and *ok = false.
int toInt(std::string_view s, bool *ok = nullptr);
// The longest prefix of at most maxBytes bytes that ends between two characters.
std::string_view truncateUtf8(std::string_view s, size_t maxBytes);
} // namespace bmc::text
