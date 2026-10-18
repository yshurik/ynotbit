#include <bmc/records.h>
#include <bmc/text.h>

namespace bmc {
std::string channelName(std::string_view label) {
    auto name = text::trim(label);
    if (name.substr(0, 7) == "[chan] ")
        name = text::trim(name.substr(7));
    if (name.empty() || name.front() == '#')
        return std::string(name);
    return "#" + std::string(name);
}
} // namespace bmc
