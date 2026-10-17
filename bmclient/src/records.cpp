#include <bmc/records.h>
#include <bmc/text.h>

namespace bmc {
namespace {
char32_t codepoint(std::string_view s, size_t i, size_t n) {
    const auto b = [&](size_t k) { return uint8_t(s[i + k]); };
    char32_t c = b(0);
    if (n == 2)
        return (c & 0x1f) << 6 | (b(1) & 0x3f);
    if (n == 3)
        return (c & 0x0f) << 12 | (b(1) & 0x3f) << 6 | (b(2) & 0x3f);
    if (n == 4)
        return (c & 0x07) << 18 | (b(1) & 0x3f) << 12 | (b(2) & 0x3f) << 6 | (b(3) & 0x3f);
    return c;
}
// QString::trimmed(): drop the closest white space (QChar::isSpace) at both ends. A byte that is
// not part of a well-formed sequence is not white space, so it stops the trim.
std::string_view trim(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size()) {
        const size_t n = text::sequenceLength(s.substr(begin));
        if (n == 0 || !text::isSpace(codepoint(s, begin, n)))
            break;
        begin += n;
    }
    size_t end = begin, last = begin;
    while (end < s.size()) {
        const size_t n = text::sequenceLength(s.substr(end));
        if (n == 0) {
            last = ++end;
            continue;
        }
        end += n;
        if (!text::isSpace(codepoint(s, end - n, n)))
            last = end;
    }
    return s.substr(begin, last - begin);
}
} // namespace

std::string channelName(std::string_view label) {
    auto name = trim(label);
    if (name.substr(0, 7) == "[chan] ")
        name = trim(name.substr(7));
    if (name.empty() || name.front() == '#')
        return std::string(name);
    return "#" + std::string(name);
}
} // namespace bmc
