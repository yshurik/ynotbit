#include "appearance.h"
#include "markdown_editor.h"
#include <QApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextList>

class EditorTests : public QObject {
    Q_OBJECT
private slots:
    void visualRoundTrip() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick; import QtQuick.Controls; TextArea { textFormat: TextEdit.RichText }", QUrl());
        QScopedPointer<QObject> field(component.create());
        QVERIFY2(field, qPrintable(component.errorString()));
        bm::MarkdownEditor editor;
        editor.attach(field.data());
        const QString source = "**bold** and [link](https://example.org)\n\n- one\n- two\n\n```\nconst x = 1;\n```\n";
        editor.loadMarkdown(source);
        QCOMPARE(editor.markdown(), source); // Unedited drafts retain exact wire text.
        QVERIFY(!editor.textDocument()->toPlainText().contains("**"));
        QTextCursor cursor(editor.textDocument());
        cursor.setPosition(0);
        cursor.setPosition(4, QTextCursor::KeepAnchor);
        QCOMPARE(cursor.charFormat().fontWeight(), QFont::Bold);
        field->setProperty("selectionStart", 0); // Selection is made through the real control below.
        QMetaObject::invokeMethod(field.data(), "select", Q_ARG(int, 0), Q_ARG(int, 4));
        editor.italic();
        QVERIFY(cursor.charFormat().fontItalic());
        const auto result = editor.markdown();
        QTextDocument reopened;
        reopened.setMarkdown(result);
        QVERIFY(reopened.toPlainText().contains("bold"));
        QVERIFY(reopened.toPlainText().contains("link"));
        QVERIFY(result.contains("https://example.org"));
        QVERIFY(result.contains("const x = 1;"));
        QVERIFY(reopened.findBlockByNumber(1).textList());
        editor.textDocument()->undo();
        QVERIFY(!cursor.charFormat().fontItalic());
    }
    void literalsAndImages() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick; TextEdit { textFormat: TextEdit.RichText }", QUrl());
        QScopedPointer<QObject> field(component.create());
        QVERIFY(field);
        bm::MarkdownEditor editor;
        editor.attach(field.data());
        editor.loadMarkdown("");
        QTextCursor cursor(editor.textDocument());
        cursor.insertText("**literal** <tag> [words](file:///tmp/private)");
        QTextDocument reopened;
        reopened.setMarkdown(editor.markdown());
        QVERIFY(reopened.toPlainText().contains("literal"));
        QVERIFY(reopened.toPlainText().contains("<tag>"));
        QTemporaryDir dir;
        QImage image(4, 4, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(dir.filePath("private.png")));
        auto loaded = editor.textDocument()->resource(QTextDocument::ImageResource, QUrl::fromLocalFile(dir.filePath("private.png")));
        QVERIFY(loaded.value<QImage>().isNull());
        QVERIFY(!editor.allowedLink("file:///tmp/private"));
        QVERIFY(!editor.allowedLink("javascript:alert(1)"));
        QVERIFY(editor.allowedLink("https://example.org/path"));
    }
    void themes() {
        bm::Appearance appearance;
        appearance.setMode("dark");
        QVERIFY(appearance.dark());
        QVERIFY(QApplication::palette().color(QPalette::Base).lightness() < 80);
        appearance.setMode("light");
        QVERIFY(!appearance.dark());
        QVERIFY(QApplication::palette().color(QPalette::Base).lightness() > 200);
        bm::Appearance reopened;
        QCOMPARE(reopened.mode(), QString("light"));
        appearance.setMode("system");
        QCOMPARE(appearance.mode(), QString("system"));
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("YnotbitTests");
    app.setApplicationName("Editor");
    EditorTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "editor_tests.moc"
