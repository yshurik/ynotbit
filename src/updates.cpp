#include "updates.h"
#include "qt_bridge.h"
#include <QCoreApplication>
#include <QStringList>
#include <bmc/updates.h>

namespace bm::updates {
namespace {
const QString kDefaultNickbotAddress = "BM-87nickbot3AwHg3ZfcuZaRpHiCH9Kdk4Z5z";
QList<DefaultSubscription> &defaults() {
    static QList<DefaultSubscription> list{
        // Weekly news of the network: chans, tools, subscriptions worth following.
        {digestAddress(), QT_TRANSLATE_NOOP("bm::Session", "Bitmessage digest")},
    };
    return list;
}
QList<DefaultContact> &contactDefaults() {
    static QList<DefaultContact> list{
        // The chat responder run for ynotbit, a vanity address (BM-87echo...): it answers a chat
        // with what it was sent.
        {"BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8", "/echobot", "/echobot"},
        // The nicknames service run for ynotbit (docs/nickbot.md), a vanity address (BM-87nickbot...)
        // found with tools/vanity: it runs from the address's two keys, not from a phrase. Replacing
        // it is this line, the one below and nickbot/seed.example.txt.
        {kDefaultNickbotAddress, "/nickbot", "/nickbot"},
    };
    return list;
}
QList<DefaultNickbot> &nickbotDefaults() {
    static QList<DefaultNickbot> list{{kDefaultNickbotAddress, "nickbot"}};
    return list;
}
} // namespace
QList<DefaultContact> defaultContacts() {
    return contactDefaults();
}
void setDefaultContactsForTesting(const QList<DefaultContact> &contacts) {
    contactDefaults() = contacts;
}
QList<DefaultNickbot> defaultNickbots() {
    return nickbotDefaults();
}
void setDefaultNickbotsForTesting(const QList<DefaultNickbot> &bots) {
    nickbotDefaults() = bots;
}
QString digestAddress() {
    return bridge::fromStd(bmc::updates::digestAddress());
}
QList<DefaultSubscription> defaultSubscriptions() {
    return defaults();
}
void setDefaultSubscriptionsForTesting(const QList<DefaultSubscription> &subscriptions) {
    defaults() = subscriptions;
}
QString publisherAddress() {
    return bridge::fromStd(bmc::updates::publisherAddress());
}
void setPublisherAddressForTesting(const QString &address) {
    bmc::updates::setPublisherAddressForTesting(bridge::toStd(address));
}
std::optional<QString> announcedVersion(const QString &subject) {
    const auto version = bmc::updates::announcedVersion(bridge::toStd(subject));
    if (!version)
        return {};
    return bridge::fromStd(*version);
}
QString releaseUrl(const QString &version) {
    return bridge::fromStd(bmc::updates::releaseUrl(bridge::toStd(version)));
}
int compareVersions(const QString &a, const QString &b) {
    return bmc::updates::compareVersions(bridge::toStd(a), bridge::toStd(b));
}
} // namespace bm::updates
