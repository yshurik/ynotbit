#pragma once
#include <QString>
#include <optional>
namespace bm::updates {
// The ynotbit release announcements: broadcasts signed by one publisher
// address. Every mailbox hears them without subscribing, unless turned off.
QString publisherAddress();
void setPublisherAddressForTesting(const QString &address);
inline const QString kNotifySetting = "updates:notify"; // "off" turns notices off
inline const QString kLatestSetting = "updates:latest"; // newest announced version
inline const QString kDismissedSetting = "updates:dismissed"; // a version
// The version a subject such as "ynotbit 0.5.2" announces.
std::optional<QString> announcedVersion(const QString &subject);
// The release page; built here, never taken from a letter.
QString releaseUrl(const QString &version);
// <0, 0, >0 like strcmp, numerically per dotted part ("0.10" > "0.9").
int compareVersions(const QString &a, const QString &b);
} // namespace bm::updates
