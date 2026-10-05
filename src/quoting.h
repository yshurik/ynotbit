#pragma once
#include <QString>

// Quoting in replies and in the reader. ynotbit quotes email-style with ">"
// lines; PyBitmessage by default stacks history below a line of 54 dashes
// instead. Both are read; replies are written with ">".
namespace bm {
// The Markdown signature new letters and replies start with.
inline const QString kSignature = QStringLiteral("-- sent by y*notbit*");
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
// line one level deeper, without the sender's signature (a "-- " line, or
// kSignature).
QString quoteForReply(const QString &body, const QString &attribution);
} // namespace bm
