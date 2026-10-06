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
// A letter's document: it shows the pictures the letter carries (data: URLs,
// see letterImage) and never fetches anything.
class SafeDocument : public QTextDocument {
  public:
    using QTextDocument::QTextDocument;
    QVariant loadResource(int type, const QUrl &name) override;
    // The next letter: the last one's pictures are forgotten.
    void clear() override {
        pictures_.clear();
        keys_.clear();
        QTextDocument::clear();
    }
    // Read-only letters: gives each picture a short name and its size for the
    // text width, so laying out and painting never decode a picture or parse
    // its data: URL. Again whenever the width changes. (The composer keeps the
    // data: URLs, which it writes out.)
    void fitPictures();

  private:
    struct Picture {
        QString url;
        QSize size; // upright, from its header
    };
    // The picture a name stands for: a short name from fitPictures, or a
    // data: URL, met here first. Null for anything not shown.
    const Picture *picture(const QString &name, QString *key);
    int pictureRoom() const;
    QHash<QString, Picture> pictures_; // by key, a digest of the URL
    QHash<QString, QString> keys_;     // data: URL -> key, empty if refused
};
// How many pictures have been decoded for showing so far.
int pictureDecodes();
// A letter view: a text browser that draws quote bars.
class LetterView : public QTextBrowser {
  public:
    using QTextBrowser::QTextBrowser;

  protected:
    void paintEvent(QPaintEvent *event) override {
        QTextBrowser::paintEvent(event);
        paintQuoteBars(this);
    }
    void resizeEvent(QResizeEvent *event) override {
        QTextBrowser::resizeEvent(event);
        if (auto doc = dynamic_cast<SafeDocument *>(document()))
            doc->fitPictures();
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
