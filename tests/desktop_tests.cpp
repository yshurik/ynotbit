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
        // Reopen an existing draft, edit it, and activate the actual QML send control.
        auto draft = session.messages().first().toMap();
        auto ownAddress = session.identities().first().toMap().value("address").toString();
        draft["to"] = ownAddress;
        require(QMetaObject::invokeMethod(window, "editLetter", Q_ARG(QVariant, QVariant(draft)),
                                          Q_ARG(QVariant, QVariant(false))),
                "open draft editor");
        QTest::qWait(20);
        auto *sendButton = window->findChild<QObject *>("sendButton");
        auto *bodyField = window->findChild<QObject *>("bodyField");
        require(sendButton && bodyField, "send controls exist");
        bodyField->setProperty("text", "Edited through the composer");
        require(sendButton->property("enabled").toBool(),
                "send control enabled with sender and recipient");
        require(QMetaObject::invokeMethod(sendButton, "clicked"), "activate send control");
        require(session.messages().size() == 1, "editing does not duplicate draft");
        auto outgoing = session.messages().first().toMap();
        require(outgoing.value("folder") == "Outbox" &&
                    outgoing.value("body") == "Edited through the composer",
                "send saves edits and queues letter");
        session.cancelLetter(outgoing.value("hash").toString());
        require(session.messages().first().toMap().value("state") == "cancelled",
                "desktop cancellation");
        // Lock flushes the editor immediately, before its debounce timer fires.
        require(QMetaObject::invokeMethod(window, "editLetter",
                                          Q_ARG(QVariant, QVariant(QVariantMap())),
                                          Q_ARG(QVariant, QVariant(false))),
                "open new editor");
        bodyField->setProperty("text", "Saved when locking immediately");
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
        require(session.mailboxOpen() && session.messages().size() == 2,
                "unlock restores sent draft and draft flushed before lock");
        session.lock();
        std::cout << "PASS: desktop file/password dialogs, identity, encrypted draft, lock clears "
                     "views, failed unlock, document reopen\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
