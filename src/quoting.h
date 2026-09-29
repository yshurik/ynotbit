#pragma once
#include <QString>

// Quoting in replies and in the reader. ynotbit quotes email-style with ">"
// lines; PyBitmessage by default stacks history below a line of 54 dashes
// instead. Both are read; replies are written with ">".
namespace bm {
// PyBitmessage's reply separator: a line of exactly 54 dashes.
bool isPyBitmessageSeparator(const QString &line);
// How many ">" markers a line starts with ("> > x", ">>x" and "> x" all
// count), and optionally how many characters they take, including the one
// space after the last marker.
int quoteLevel(const QString &line, int *markerLength = nullptr);
// The body with every line's leading ">" markers removed.
QString withoutQuoteMarkers(const QString &body);
// Whether a letter quotes anything, either way.
bool hasQuoting(const QString &body);
// The body with PyBitmessage's separator-stacked history rewritten as ">"
// quoting: the letter below the n-th separator is n levels deep.
QString normalizeQuotes(const QString &body);
// The original letter as a reply quotes it: an attribution line, then every
// line one level deeper, without the sender's "-- " signature.
QString quoteForReply(const QString &body, const QString &attribution);
// Markdown for the composer: quoted lines end in hard breaks, so each keeps
// its own line instead of merging into one paragraph.
QString withQuoteLineBreaks(const QString &markdown);
// The composer's Markdown export writes each quoted line as a paragraph of its
// own ("> a\n>\n> b"); this joins them back into consecutive lines ("> a  \n> b").
QString tightenQuotes(const QString &markdown);
// A letter body as Markdown for the composer's editor: quoted lines keep
// their line breaks, and the "-- " signature delimiter keeps its own line.
QString toComposerMarkdown(const QString &body);
// The composer's Markdown export as the letter body: quoted lines joined back
// (tightenQuotes) and the "-- " delimiter un-escaped -- the export writes it
// as "\--", which reached recipients literally.
QString fromComposerMarkdown(const QString &markdown);
} // namespace bm
