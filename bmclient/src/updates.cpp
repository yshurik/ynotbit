#include <algorithm>
#include <bmc/text.h>
#include <bmc/updates.h>
#include <mutex>
#include <vector>

namespace bmc::updates {
namespace {
std::mutex lock;
std::string &publisher() {
    // Signs ynotbit's release announcements; its key is held by the maintainer.
    static std::string address = "BM-2666hf5eAbjCJMaPwC7eG3Um55QJGM";
    return address;
}
bool space(char c) { // \s of the regular expression: the space, \t \n \v \f \r
    return c == ' ' || (c >= '\t' && c <= '\r');
}
bool digit(char c) {
    return c >= '0' && c <= '9';
}
char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}
} // namespace

std::string publisherAddress() {
    std::lock_guard guard(lock);
    return publisher();
}

void setPublisherAddressForTesting(std::string address) {
    std::lock_guard guard(lock);
    publisher() = std::move(address);
}

std::string digestAddress() {
    // Signs the digest; its key is held by the maintainer.
    return "BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9";
}

std::optional<std::string> announcedVersion(std::string_view s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n && space(s[i]))
        ++i;
    static constexpr std::string_view word = "ynotbit";
    if (n - i < word.size())
        return std::nullopt;
    for (size_t k = 0; k < word.size(); ++k)
        if (lower(s[i + k]) != word[k])
            return std::nullopt;
    i += word.size();
    const size_t gap = i;
    while (i < n && space(s[i]))
        ++i;
    if (i == gap) // \s+ needs at least one
        return std::nullopt;
    if (i < n && lower(s[i]) == 'v')
        ++i;
    const size_t start = i;
    const auto group = [&] { // \d{1,4}: a fifth digit leaves no match
        const size_t from = i;
        while (i < n && digit(s[i]))
            ++i;
        return i - from >= 1 && i - from <= 4;
    };
    if (!group())
        return std::nullopt;
    int more = 0;
    while (i < n && s[i] == '.' && more < 3) {
        ++i;
        if (!group())
            return std::nullopt;
        ++more;
    }
    if (more == 0)
        return std::nullopt;
    const size_t end = i;
    while (i < n && space(s[i]))
        ++i;
    if (i != n)
        return std::nullopt;
    return std::string(s.substr(start, end - start));
}

std::string releaseUrl(std::string_view version) {
    return "https://github.com/yshurik/ynotbit/releases/tag/v" + std::string(version);
}

int compareVersions(std::string_view a, std::string_view b) {
    const auto split = [](std::string_view s) {
        std::vector<std::string_view> parts; // "" gives one empty part, as QString::split did
        size_t from = 0;
        for (;;) {
            const size_t dot = s.find('.', from);
            parts.push_back(s.substr(from, dot == std::string_view::npos ? dot : dot - from));
            if (dot == std::string_view::npos)
                break;
            from = dot + 1;
        }
        return parts;
    };
    const auto x = split(a), y = split(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? text::toInt(x[i]) : 0,
                  q = i < y.size() ? text::toInt(y[i]) : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}
} // namespace bmc::updates
