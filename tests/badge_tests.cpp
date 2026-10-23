// The number on the app's icon: given to the system when it changes, and only while it is on.
#include "app_badge.h"
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <vector>
static void require(bool ok, const char *message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        throw std::runtime_error(message);
    }
}
int main(int argc, char **argv) {
    // Settings go in a folder of their own, never the real one (macOS and Windows ignore the
    // environment for that; the path does it everywhere).
    QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    QCoreApplication app(argc, argv);
    app.setOrganizationName("YnotbitTests");
    app.setApplicationName("Badge");
    try {
        std::vector<qint64> given;
        bm::AppBadge::setSinkForTesting([&](qint64 number) { given.push_back(number); });
        require(bm::AppBadge::enabled(), "the badge is on until the user switches it off");
        bm::AppBadge::set(0);
        require(given.empty(), "no number is shown, so none is given to clear");
        bm::AppBadge::set(5);
        require(given == std::vector<qint64>{5}, "a number is given to the system");
        bm::AppBadge::set(5);
        require(given.size() == 1, "the same number is not given twice");
        bm::AppBadge::set(7);
        bm::AppBadge::set(0);
        require(given == (std::vector<qint64>{5, 7, 0}), "a change is given, and zero clears");
        bm::AppBadge::set(-3);
        require(given.size() == 3, "a number below zero is zero");
        // Off: the number is taken away and stays away while the window goes on saying it.
        bm::AppBadge::set(4);
        require(given.back() == 4, "test sanity: the number is shown");
        bm::AppBadge::setEnabled(false);
        require(!bm::AppBadge::enabled() && given.back() == 0,
                "switched off, the badge is cleared");
        const auto cleared = given.size();
        bm::AppBadge::set(9);
        require(given.size() == cleared, "and stays clear while it is off");
        bm::AppBadge::setEnabled(true);
        require(given.back() == 9, "switched on again, it shows what the window last said");
        // The choice is kept for the next run.
        bm::AppBadge::setEnabled(false);
        require(!QSettings().value("badge/enabled", true).toBool(),
                "the choice is in the settings");
        bm::AppBadge::setEnabled(true);
        require(QSettings().value("badge/enabled", false).toBool(),
                "and so is the choice to have it");
        // The app ending takes the number off, once.
        bm::AppBadge::clear();
        require(given.back() == 0, "the number is taken off when the app ends");
        const auto ended = given.size();
        bm::AppBadge::clear();
        require(given.size() == ended, "and not again");
        bm::AppBadge::set(2);
        require(given.back() == 2, "a number set after that is shown again");
        // The app going out of its event loop takes the number off by itself: a Linux launcher
        // would go on showing it.
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        app.exec();
        require(given.back() == 0, "quitting takes the number off");
    } catch (const std::exception &) {
        return 1;
    }
    std::cout << "PASS: the app badge\n";
    return 0;
}
