#pragma once
#include <QString>
#include <bmc/updates.h>
#include <optional>
namespace bm::updates {
// The ynotbit release announcements: broadcasts signed by one publisher
// address. Every mailbox hears them without subscribing, unless turned off.
QString publisherAddress();
void setPublisherAddressForTesting(const QString &address);
// The keys the delivery engine reads: one spelling, in the core, so a bot and
// the app cannot drift apart.
inline const QString kNotifySetting = QString::fromStdString(bmc::updates::kNotifySetting);
inline const QString kLatestSetting = QString::fromStdString(bmc::updates::kLatestSetting);
inline const QString kDismissedSetting = "updates:dismissed"; // a version
// Subscriptions every mailbox starts with: added once when a mailbox first
// opens, so unsubscribing sticks. The label is untranslated (Session::tr).
struct DefaultSubscription {
    QString address;
    const char *label;
};
QList<DefaultSubscription> defaultSubscriptions();
// "Bitmessage digest": weekly news of the network, one of the defaults.
QString digestAddress();
void setDefaultSubscriptionsForTesting(const QList<DefaultSubscription> &subscriptions);
inline const QString kSeededSetting = "seeded:"; // + address: offered once already
// Contacts every mailbox starts with: added once when it first opens, or is made, so deleting one
// sticks. The nickname is the handle the address goes by (/bot, @person, #chan); the label is the
// name it is saved under.
struct DefaultContact {
    QString address, nickname, label;
};
QList<DefaultContact> defaultContacts(); // the echo bot
void setDefaultContactsForTesting(const QList<DefaultContact> &contacts);
inline const QString kSeededContactSetting = "seeded-contact:"; // + address: added once already
// An address's nickname is kept in the mailbox's settings, not chosen by the user.
inline const QString kNicknameSetting = "nickname:"; // + address
// The version a subject such as "ynotbit 0.5.2" announces.
std::optional<QString> announcedVersion(const QString &subject);
// The release page; built here, never taken from a letter.
QString releaseUrl(const QString &version);
// <0, 0, >0 like strcmp, numerically per dotted part ("0.10" > "0.9").
int compareVersions(const QString &a, const QString &b);
} // namespace bm::updates
