#include "updates.h"
#include "qt_bridge.h"
#include <QCoreApplication>
#include <QStringList>
#include <bmc/updates.h>

namespace bm::updates {
namespace {
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
    };
    return list;
}
} // namespace
QList<DefaultContact> defaultContacts() {
    return contactDefaults();
}
void setDefaultContactsForTesting(const QList<DefaultContact> &contacts) {
    contactDefaults() = contacts;
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
