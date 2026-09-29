#include "session.h"
#include "portable_relay.h"
#include "appearance.h"
#include "desktop_window.h"
#include "i18n.h"
#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>
extern "C" {
int ntb_daemon(int argc, char **argv);
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--qt-node")
        return bm::runPortableRelay(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--node")
        return ntb_daemon(argc - 1, argv + 1);
    QApplication app(argc, argv);
    app.setOrganizationName("NotbitDesktop");
    app.setApplicationName("Notbit Desktop");
    app.setApplicationDisplayName("ynotbit");
    app.setApplicationVersion(YNOTBIT_VERSION);
    app.setDesktopFileName("ynotbit");
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
        bm::Session session(root, args.contains("--offline"));
        bm::DesktopWindow window(session);
        window.show();
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
