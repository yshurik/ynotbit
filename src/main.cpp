#include "session.h"
#include <QApplication>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>
#ifdef Q_OS_UNIX
extern "C" {
int ntb_daemon(int argc, char **argv);
}
#endif
int main(int argc, char **argv) {
#ifdef Q_OS_UNIX
    if (argc > 1 && std::string(argv[1]) == "--node")
        return ntb_daemon(argc - 1, argv + 1);
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("NotbitDesktop");
    app.setApplicationName("Notbit Desktop");
    app.setApplicationDisplayName("ynotbit");
    app.setApplicationVersion("0.2.0-dev");
    QQuickStyle::setStyle("Basic");
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
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        if (engine.rootObjects().isEmpty())
            return 1;
        index = args.indexOf("--screenshot");
        if (index >= 0 && index + 1 < args.size()) {
            auto path = args[index + 1];
            QTimer::singleShot(1200, &app, [&, path] {
                auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
                if (window)
                    window->grabWindow().save(path);
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
