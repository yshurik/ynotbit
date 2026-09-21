#include "session.h"
#include "portable_relay.h"
#include "appearance.h"
#include "desktop_window.h"
#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>
#ifdef Q_OS_UNIX
extern "C" {
int ntb_daemon(int argc, char **argv);
}
#endif
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--qt-node")
        return bm::runPortableRelay(argc, argv);
#ifdef Q_OS_UNIX
    if (argc > 1 && std::string(argv[1]) == "--node")
        return ntb_daemon(argc - 1, argv + 1);
#else
    if (argc > 1 && std::string(argv[1]) == "--node")
        return bm::runPortableRelay(argc, argv);
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("NotbitDesktop");
    app.setApplicationName("Notbit Desktop");
    app.setApplicationDisplayName("ynotbit");
    app.setApplicationVersion("0.4.7");
    app.setWindowIcon(bm::appLogo());
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
