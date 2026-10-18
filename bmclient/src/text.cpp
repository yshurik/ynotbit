#include <bmc/text.h>
#include <cstdint>

namespace bmc::text {
size_t sequenceLength(std::string_view s) {
    if (s.empty())
        return 0;
    const auto byte = [&](size_t i) { return uint8_t(s[i]); };
    const uint8_t lead = byte(0);
    if (lead < 0x80)
        return 1;
    size_t need;
    uint8_t low = 0x80, high = 0xbf; // what the second byte may be
    if (lead >= 0xc2 && lead <= 0xdf) {
        need = 2;
    } else if (lead == 0xe0) {
        need = 3;
        low = 0xa0;
    } else if (lead == 0xed) {
        need = 3;
        high = 0x9f;
    } else if (lead >= 0xe1 && lead <= 0xef) {
        need = 3;
    } else if (lead == 0xf0) {
        need = 4;
        low = 0x90;
    } else if (lead == 0xf4) {
        need = 4;
        high = 0x8f;
    } else if (lead >= 0xf1 && lead <= 0xf3) {
        need = 4;
    } else {
        return 0;
    }
    if (s.size() < need || byte(1) < low || byte(1) > high)
        return 0;
    for (size_t i = 2; i < need; ++i)
        if ((byte(i) & 0xc0) != 0x80)
            return 0;
    return need;
}

std::string sanitizeUtf8(std::string_view in) {
    size_t i = 0;
    if (in.size() >= 3 && in.compare(0, 3, "\xEF\xBB\xBF") == 0)
        i = 3;
    std::string out;
    out.reserve(in.size());
    while (i < in.size()) {
        if (const size_t n = sequenceLength(in.substr(i))) {
            out.append(in.substr(i, n));
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

bool validUtf8(std::string_view bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const size_t n = sequenceLength(bytes.substr(i));
        if (n == 0)
            return false;
        i += n;
    }
    return true;
}

bool isSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000;
}

bool isBlank(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = sequenceLength(s.substr(i));
        if (n == 0)
            return false;
        const auto cont = [&](size_t k) { return char32_t(uint8_t(s[i + k]) & 0x3f); };
        char32_t c = uint8_t(s[i]);
        if (n == 2)
            c = ((c & 0x1f) << 6) | cont(1);
        else if (n == 3)
            c = ((c & 0x0f) << 12) | (cont(1) << 6) | cont(2);
        else if (n == 4)
            c = ((c & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
        if (!isSpace(c))
            return false;
        i += n;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size()) {
        const size_t n = sequenceLength(s.substr(begin));
        if (n == 0)
            break;
        const auto cont = [&](size_t k) { return char32_t(uint8_t(s[begin + k]) & 0x3f); };
        char32_t c = uint8_t(s[begin]);
        if (n == 2)
            c = ((c & 0x1f) << 6) | cont(1);
        else if (n == 3)
            c = ((c & 0x0f) << 12) | (cont(1) << 6) | cont(2);
        else if (n == 4)
            c = ((c & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
        if (!isSpace(c))
            break;
        begin += n;
    }
    size_t end = begin, last = begin;
    while (end < s.size()) {
        const size_t n = sequenceLength(s.substr(end));
        if (n == 0) {
            last = ++end;
            continue;
        }
        const auto cont = [&](size_t k) { return char32_t(uint8_t(s[end + k]) & 0x3f); };
        char32_t c = uint8_t(s[end]);
        if (n == 2)
            c = ((c & 0x1f) << 6) | cont(1);
        else if (n == 3)
            c = ((c & 0x0f) << 12) | (cont(1) << 6) | cont(2);
        else if (n == 4)
            c = ((c & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
        end += n;
        if (!isSpace(c))
            last = end;
    }
    return s.substr(begin, last - begin);
}

int toInt(std::string_view s, bool *ok) {
    const auto fail = [&] {
        if (ok)
            *ok = false;
        return 0;
    };
    const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    size_t begin = 0, end = s.size();
    while (begin < end && space(s[begin]))
        ++begin;
    while (end > begin && space(s[end - 1]))
        --end;
    bool negative = false;
    if (begin < end && (s[begin] == '+' || s[begin] == '-'))
        negative = s[begin++] == '-';
    if (begin == end)
        return fail();
    int64_t value = 0;
    for (; begin < end; ++begin) {
        if (s[begin] < '0' || s[begin] > '9')
            return fail();
        value = value * 10 + (s[begin] - '0');
        if (value > (int64_t(1) << 31))
            return fail();
    }
    if (negative)
        value = -value;
    if (value > INT32_MAX || value < INT32_MIN)
        return fail();
    if (ok)
        *ok = true;
    return int(value);
}

std::string_view truncateUtf8(std::string_view s, size_t maxBytes) {
    if (s.size() <= maxBytes)
        return s;
    size_t n = maxBytes;
    while (n > 0 && (uint8_t(s[n]) & 0xc0) == 0x80)
        --n;
    return s.substr(0, n);
}

std::u16string utf16FromUtf8(std::string_view in) {
    std::u16string out;
    size_t i = 0;
    while (i < in.size()) {
        const size_t n = sequenceLength(in.substr(i));
        if (n == 0) {
            out.push_back(0xfffd); // one replacement per invalid byte, as QString::fromUtf8
            ++i;
            continue;
        }
        const auto b = [&](size_t k) { return uint8_t(in[i + k]); };
        char32_t c = b(0);
        if (n == 2)
            c = (c & 0x1f) << 6 | (b(1) & 0x3f);
        else if (n == 3)
            c = (c & 0x0f) << 12 | (b(1) & 0x3f) << 6 | (b(2) & 0x3f);
        else if (n == 4)
            c = (c & 0x07) << 18 | (b(1) & 0x3f) << 12 | (b(2) & 0x3f) << 6 | (b(3) & 0x3f);
        if (c <= 0xffff) {
            out.push_back(char16_t(c));
        } else {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        }
        i += n;
    }
    return out;
}

std::string utf8FromUtf16(std::u16string_view in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t c = in[i];
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 < in.size() && in[i + 1] >= 0xdc00 && in[i + 1] <= 0xdfff)
                c = 0x10000 + ((c - 0xd800) << 10) + (in[++i] - 0xdc00);
            else {
                out += '?';
                continue;
            }
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            out += '?';
            continue;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xf0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3f));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }
    return out;
}
} // namespace bmc::text
