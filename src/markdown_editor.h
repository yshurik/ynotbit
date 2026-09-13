#pragma once
#include <QObject>
#include <QPointer>
#include <QTextCursor>
#include <QTextDocument>

namespace bm {
// The visual document is GUI-thread confined. Only Markdown strings cross into storage.
class MarkdownEditor : public QObject {
    Q_OBJECT
public:
    explicit MarkdownEditor(QObject *parent = nullptr);
    Q_INVOKABLE void attach(QObject *textEdit);
    Q_INVOKABLE void loadMarkdown(const QString &markdown);
    Q_INVOKABLE QString markdown() const;
    Q_INVOKABLE void bold();
    Q_INVOKABLE void italic();
    Q_INVOKABLE void heading(int level);
    Q_INVOKABLE void list(bool ordered = false);
    Q_INVOKABLE void quote();
    Q_INVOKABLE void code();
    Q_INVOKABLE void link(const QString &url);
    Q_INVOKABLE void pastePlainText();
    Q_INVOKABLE bool allowedLink(const QString &url) const;
    QTextDocument *textDocument() const { return document_; }
signals:
    void contentChanged();
private:
    QTextCursor cursor() const;
    void characterFormat(const QTextCharFormat &format);
    QPointer<QObject> edit_;
    QPointer<QTextDocument> document_;
    QString original_;
    bool loading_ = false;
    bool edited_ = false;
};
}
