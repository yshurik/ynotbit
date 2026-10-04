#pragma once
#include <QColor>
#include <QImage>
#include <QString>

class QTextBlock;
class QTextDocument;
class QUrl;

// A letter body as a rich text document and back, for the reader and the
// composer. Qt's own Markdown import and export lose quoting (quoted headings
// come back unquoted, the line after a list joins the list), so ynotbit reads
// each quote level on its own and writes its Markdown itself.
//
// The dialect is email-flavoured Markdown: a newline inside a paragraph is a
// line break (a plain-text letter keeps its lines), blank lines separate
// paragraphs, and ">" marks quoting, one per level.
namespace bm {
// Quote styling: an indent per level, a coloured bar (painted by the view)
// and a faint tint in the level's colour.
constexpr int kQuoteIndent = 14;
QColor quoteColor(int level);
void setQuoteLevel(QTextBlock block, int level);
int blockQuoteLevel(const QTextBlock &block);

// Fills the document from a body. With markdown false every paragraph is
// plain text; either way quoting becomes block quote levels.
void loadLetter(QTextDocument *doc, const QString &body, bool markdown);
// The document back as a body. loadLetter(markdown = true) of the result
// gives back the same document.
QString letterMarkdown(const QTextDocument *doc);

// Pictures travel inside the letter as data: URLs (RFC 2397), the standard
// Markdown way: "![alt][img1]" in the text and "[img1]: data:image/png;base64,..."
// at the end. Only PNG, JPEG, GIF and WebP load, within size limits; nothing
// is ever fetched.
constexpr int kMaxLetterImageSide = 4096;
// The most one inserted picture takes of a letter (about 160 KB of its
// ~255 KB): room left for words, and a shorter proof of work.
constexpr int kMaxLetterPicture = 160 * 1024;
QImage letterImage(const QUrl &url);
// A picture ready for a letter: redrawn (its metadata dropped) and shrunk
// until its data: URL fits in budget characters; empty if it cannot.
QString imageDataUrl(const QImage &image, int budget);
// Reference-style data: images made inline, their definitions taken out.
// Each quote level is parsed on its own, so a definition at the end of the
// letter would otherwise not reach a picture in a quote. PyBitmessage's
// <img src="data:..."> becomes a Markdown picture too.
QString inlineImageReferences(const QString &markdown);
} // namespace bm
