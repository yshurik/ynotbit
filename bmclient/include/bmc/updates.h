// The ynotbit release announcements: broadcasts signed by one publisher address.
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace bmc::updates {
std::string publisherAddress();
void setPublisherAddressForTesting(std::string address);
// "Bitmessage digest": weekly news of the network.
std::string digestAddress();
// The version a subject such as "ynotbit 0.5.2" announces:
// ^\s*ynotbit\s+v?(\d{1,4}(\.\d{1,4}){1,3})\s*$, ASCII case folding and ASCII white space (what the
// regular expression did).
std::optional<std::string> announcedVersion(std::string_view subject);
// The release page; built here, never taken from a letter.
std::string releaseUrl(std::string_view version);
// <0, 0, >0 (-1, 0, 1) like strcmp, numerically per dotted part ("0.10" > "0.9").
int compareVersions(std::string_view a, std::string_view b);
} // namespace bmc::updates
