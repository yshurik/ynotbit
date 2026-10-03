#pragma once
// How a letter is shown: body modes (plain / text / Markdown / hex), the
// subject line, identicons and the toolbar icons. Shared by the reader, the
// letter window and the subscriptions feed.
#include "appearance.h"
#include "letter_document.h"
#include "quoting.h"
#include <QIcon>
#include <QPixmap>
#include <QSyntaxHighlighter>
#include <QTextBrowser>
#include <QTextDocument>
class QBoxLayout;
class QTextEdit;
namespace bm {
bool looksCryptic(const QString &text);
bool looksCryptic(const QString &subject, const QString &body);
QString crypticLabel(const QString &hash);
// Offset / 16 byte pairs / printable text, as the hex view shows a body.
QString hexDump(const QByteArray &bytes);
QString singleLine(QString text);
QColor identiconColor(double hue);
QPixmap identiconPixmap(const QString &address, int size);
void paintQuoteBars(QTextEdit *edit);
class AddressHighlighter : public QSyntaxHighlighter {
  public:
    explicit AddressHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}
    void highlightBlock(const QString &text) override;
};
class SafeDocument : public QTextDocument {
  public:
    using QTextDocument::QTextDocument;
    QVariant loadResource(int, const QUrl &) override {
        return QVariant::fromValue(QImage());
    }
};
// A letter view: a text browser that draws quote bars.
class LetterView : public QTextBrowser {
  public:
    using QTextBrowser::QTextBrowser;

  protected:
    void paintEvent(QPaintEvent *event) override {
        QTextBrowser::paintEvent(event);
        paintQuoteBars(this);
    }
};
// How a letter's body is shown. Detected per message, overridable per message
// from the reader's view switch.
enum class BodyView { Plain, Text, Markdown, Hex };
void renderMarkdown(QTextBrowser *body, const QString &text);
bool looksLikeMarkdown(const QString &source);
BodyView detectBodyView(const QString &subject, const QString &text);
void renderBody(QTextBrowser *body, const QString &text, BodyView mode);
QTextEdit *subjectArea(QBoxLayout *layout, const QString &name);
void setSubject(QTextEdit *subject, const QString &text, BodyView mode = BodyView::Text);
QColor iconColor(bool dark);
QIcon materialIcon(const QString &name, QColor color);
} // namespace bm
