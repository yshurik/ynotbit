#include "app_badge.h"
#include <QGuiApplication>
#include <QSettings>
#include <algorithm>
namespace bm {
namespace {
int offered = 0;  // what the window last said it has
qint64 shown = 0; // what the system was last given: no badge is zero
std::function<void(qint64)> &sink() {
    static std::function<void(qint64)> where;
    return where;
}
bool &on() {
    static bool value = QSettings().value("badge/enabled", true).toBool();
    return value;
}
// The number goes with the app: a Linux launcher would go on showing it.
void takeOffWhenTheAppEnds() {
    static bool done = false;
    if (done || !QCoreApplication::instance())
        return;
    done = true;
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     [] { AppBadge::clear(); });
}
void show(qint64 number) {
    if (number == shown)
        return;
    if (number)
        takeOffWhenTheAppEnds();
    shown = number;
    if (sink())
        sink()(number);
    else if (qGuiApp)
        qGuiApp->setBadgeNumber(number);
}
} // namespace
void AppBadge::set(int count) {
    offered = std::max(0, count);
    show(on() ? offered : 0);
}
void AppBadge::clear() {
    offered = 0;
    show(0);
}
bool AppBadge::enabled() {
    return on();
}
void AppBadge::setEnabled(bool value) {
    on() = value;
    QSettings().setValue("badge/enabled", value);
    show(value ? offered : 0);
}
void AppBadge::setSinkForTesting(std::function<void(qint64)> where) {
    sink() = std::move(where);
}
} // namespace bm
