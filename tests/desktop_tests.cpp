#include "session.h"
#include <QApplication>
#include <QFileDialog>
#include <QInputDialog>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <iostream>
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("NotbitTests");
    app.setApplicationName("DesktopLifecycle");
    QQuickStyle::setStyle("Basic");
    QTemporaryDir dir;
    try {
        QString file, answer = "a private test password";
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, [&] {
            for (auto *w : QApplication::topLevelWidgets()) {
                if (!w->isVisible())
                    continue;
                if (auto *d = qobject_cast<QFileDialog *>(w)) {
                    d->selectFile(file);
                    QMetaObject::invokeMethod(d, "accept", Qt::QueuedConnection);
                } else if (auto *d = qobject_cast<QInputDialog *>(w)) {
                    d->setTextValue(answer);
                    d->accept();
                }
            }
        });
        responder.start(25);
        bm::Session session(dir.filePath("node"), true);
        file = dir.filePath("test.bmvault");
        session.createVault();
        require(session.unlocked(), session.error().toUtf8().constData());
        file = dir.filePath("letters.bmmail");
        session.createMailbox();
        require(session.mailboxOpen(), session.error().toUtf8().constData());
        answer = "Personal";
        session.addIdentity();
        require(session.identities().size() == 1, "identity action");
        session.saveDraft("BM-recipient", "A letter for later",
                          "A place for thoughtful correspondence.\n\nThis draft is stored inside "
                          "the encrypted mailbox. Locking the vault closes the mailbox while "
                          "network objects continue to be cached.");
        require(session.messages().size() == 1, "draft action");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        engine.load(QUrl("qrc:/ui/Main.qml"));
        require(!engine.rootObjects().isEmpty(), "desktop QML load");
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        require(window, "desktop window");
        window->setProperty("folder", "Drafts");
        window->setProperty("selected", session.messages().first());
        require(!window->property("selected").toMap().isEmpty(), "selected letter prepared");
        QTest::qWait(200);
        if (app.arguments().size() > 1)
            require(window->grabWindow().save(app.arguments()[1]), "preview capture");
        session.lock();
        QTest::qWait(50);
        require(!session.unlocked() && !session.mailboxOpen() && session.messages().isEmpty() &&
                    session.identities().isEmpty(),
                "lock clears desktop access");
        require(window->property("selected").toMap().isEmpty(),
                "lock clears selected plaintext view");
        answer = "wrong password";
        session.unlockVault();
        require(!session.unlocked() && !session.error().isEmpty(),
                "wrong password rejected in desktop");
        answer = "a private test password";
        session.unlockVault();
        require(session.mailboxOpen() && session.messages().size() == 1,
                "unlock restores document and message list");
        session.lock();
        std::cout << "PASS: desktop file/password dialogs, identity, encrypted draft, lock clears "
                     "views, failed unlock, document reopen\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
