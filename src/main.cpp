#include "session.h"
#include "appearance.h"
#include "desktop_window.h"
#include "i18n.h"
#include "mac_integration.h"
#include "single_instance.h"
#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include "desktop_entry.h"
#include <QWindow>
#include <QTimer>
#include <iostream>
extern "C" {
int ntb_daemon(int argc, char **argv);
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--node")
        return ntb_daemon(argc - 1, argv + 1);
    QApplication app(argc, argv);
    app.setOrganizationName("NotbitDesktop");
    app.setApplicationName("Notbit Desktop");
    app.setApplicationDisplayName("ynotbit");
    app.setApplicationVersion(YNOTBIT_VERSION);
    app.setDesktopFileName("ynotbit");
#ifdef Q_OS_LINUX
    // A panel without a desktop entry for the app names it by its windows' class, which Qt takes
    // from the application name when the first window is made, and keeps. The name stays what
    // the data folder and the settings know it by; for that one moment it is the app and its version.
    {
        app.setApplicationName(QString("ynotbit-") + YNOTBIT_VERSION);
        QWindow().create();
        app.setApplicationName("Notbit Desktop");
    }
#endif
#ifdef Q_OS_LINUX
    // Before the first window, which the shell names and draws from the entry when it appears.
    if (!app.arguments().contains("--portable") && !app.arguments().contains("--screenshot") &&
        qEnvironmentVariableIsEmpty("YNOTBIT_NO_DESKTOP_ENTRY")) {
        const auto appImage = qEnvironmentVariable("APPIMAGE"); // the file, not its mount
        bm::ensureDesktopEntry(
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
            QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation),
            appImage.isEmpty() ? app.applicationFilePath() : appImage);
    }
#endif
    bm::installTranslations(bm::savedLanguage());
#ifndef Q_OS_MACOS
    app.setWindowIcon(bm::appLogo());
#endif
    // macOS uses the bundled ICNS, including its separate Retina optical masters.
    try {
        auto args = app.arguments();
        QString root =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/node";
        if (args.contains("--portable"))
            root = QDir::currentPath() + "/notbit-data/node";
        int index = args.indexOf("--data-dir");
        if (index >= 0 && index + 1 < args.size())
            root = QDir(args[index + 1]).absolutePath();
        // Another ynotbit may hold this node folder, living in the notification area with no window
        // up: it is asked to show one, and this launch has nothing more to do.
        const auto instanceKey = bm::SingleInstance::keyFor(root);
        if (bm::SingleInstance::wake(instanceKey))
            return 0;
        bm::Session session(root, args.contains("--offline"));
        bm::DesktopWindow window(session);
        bm::SingleInstance instance(instanceKey);
        QObject::connect(&instance, &bm::SingleInstance::woken, &window,
                         &bm::DesktopWindow::bringToFront);
        window.show();
        // A click on the Dock icon brings back a window that was closed into the notification area.
        QTimer::singleShot(0, &window, [&window] {
            bm::mac::onReopen([&window] { window.bringToFront(); });
        });
        index = args.indexOf("--screenshot");
        if (index >= 0 && index + 1 < args.size()) {
            auto path = args[index + 1];
            QTimer::singleShot(1200, &app, [&, path] {
                window.grab().save(path);
                app.quit();
            });
        }
        if (args.contains("--smoke-test"))
            QTimer::singleShot(1500, &app, &QCoreApplication::quit);
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
