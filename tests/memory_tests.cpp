#include "session.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <iostream>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static quint64 residentBytes() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS)
        return info.resident_size;
#endif
    return 0;
}
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QTemporaryDir dir;
    try {
        const auto vaultPath = dir.filePath("test.bmvault");
        const auto mailPath = dir.filePath("test.bmmail");
        bm::Vault vault;
        vault.create(vaultPath, "temporary memory regression password");
        auto key = vault.addMailboxKey();
        bm::Mailbox mailbox;
        mailbox.create(mailPath, key, vault.mailboxKey(key));
        for (int i = 0; i < 1000; ++i)
            mailbox.store(QString::number(i), "sender", "recipient", QString::number(i),
                          QString(10000, QChar('x')), i);
        mailbox.close();
        vault.lock();
        QDir().mkpath(dir.filePath("node"));
        {
            QSettings settings(dir.filePath("node/desktop.ini"), QSettings::IniFormat);
            settings.setValue("vault", vaultPath);
            settings.setValue("mailbox", mailPath);
        }
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (!widget->isVisible())
                    continue;
                if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
                    dialog->setTextValue("temporary memory regression password");
                    dialog->accept();
                }
            }
        });
        responder.start(10);
        bm::Session session(dir.filePath("node"), true);
        session.unlockVault();
        require(session.mailboxOpen(), "open temporary mailbox");
        require(session.messages().size() == 1000, "synthetic mailbox loaded");
        const auto original = session.messages();
        auto before = residentBytes();
        // Consumers can retain implicitly shared snapshots. An unchanged refresh must
        // not allocate a new copy of every body for each retained snapshot.
        QVector<QVariantList> snapshots;
        for (int i = 0; i < 32; ++i) {
            session.readLetter("absent-letter");
            snapshots.push_back(session.messages());
        }
        std::cout << "Refresh RSS before=" << before << " after=" << residentBytes() << '\n';
        for (const auto &snapshot : snapshots)
            require(snapshot.constData() == original.constData(),
                    "unchanged refresh duplicated mailbox bodies");
        // The real timer path must also leave the list shared when only status changes.
        QTest::qWait(850);
        require(session.messages().constData() == original.constData(),
                "idle timer rebuilt messages");
        QSignalSpy resets(&session, &bm::Session::messagesChanged);
        QSignalSpy readUpdates(&session, &bm::Session::messageRead);
        QElapsedTimer selection;
        qint64 worst = 0;
        for (int i = 0; i < 100; ++i) {
            auto id = original[i].toMap().value("hash").toString();
            selection.start();
            session.readLetter(id);
            worst = std::max(worst, selection.nsecsElapsed());
            require(!session.messages()[i].toMap().value("unread").toBool(), "read flag updated");
            require(session.messages()[i].toMap().value("body").toString().isEmpty(),
                    "message list retained a full plaintext body");
            require(session.message(id).value("body").toString().size() == 10000,
                    "targeted message load failed");
        }
        require(resets.isEmpty(), "mark-read reset the entire message list");
        require(readUpdates.size() == 100, "targeted read updates missing");
        std::cout << "100 selections among 1000 letters: worst=" << worst / 1000000.0 << "ms\n";
        // Exercise a real QML binding and delegate model under repeated status ticks.
        // The old shared `changed` notification rematerialized this model every time.
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("session", &session);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            Item {
                width: 500; height: 400
                property var rows: session.messages
                property int replacements: 0
                onRowsChanged: ++replacements
                ListView {
                    anchors.fill: parent
                    model: parent.rows
                    delegate: Text { required property var modelData; text: modelData.subject }
                }
            }
        )",
                          QUrl());
        std::unique_ptr<QObject> view(component.create());
        require(bool(view), "QML memory probe loads");
        auto replacements = view->property("replacements").toInt();
        auto qmlBefore = residentBytes();
        for (int i = 0; i < 300; ++i) {
            session.changed();
            QCoreApplication::processEvents();
        }
        require(view->property("replacements").toInt() == replacements,
                "status changes replaced the QML mailbox model");
        std::cout << "300 QML status updates RSS before=" << qmlBefore
                  << " after=" << residentBytes() << '\n';
        auto draft = session.saveLetter({}, {}, "recipient", "draft", "draft", "direct");
        session.saveLetter(draft, {}, "recipient", "edited", "replacement", "direct");
        require(session.messages().first().toMap().value("body") == "replacement",
                "edited message did not invalidate snapshot");
        session.lock();
        require(session.messages().isEmpty(), "lock did not clear messages");
        std::cout
            << "PASS: unchanged message snapshots share storage, edits refresh, lock clears\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
