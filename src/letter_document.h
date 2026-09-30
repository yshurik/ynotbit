#pragma once
#include <QColor>
#include <QString>

class QTextBlock;
class QTextDocument;

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
} // namespace bm
