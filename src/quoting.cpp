#include "quoting.h"
#include <QStringList>

namespace bm {
bool isPyBitmessageSeparator(const QString &line) {
    const auto trimmed = line.trimmed();
    return trimmed.size() == 54 && trimmed.count('-') == 54;
}
int quoteLevel(const QString &line, int *markerLength) {
    int level = 0, i = 0, end = 0;
    while (i < line.size()) {
        if (line[i] == '>') {
            ++level;
            end = ++i;
        } else if (line[i] == ' ' && level > 0 && i + 1 < line.size() && line[i + 1] == '>') {
            ++i; // "> > x": a space between markers
        } else {
            break;
        }
    }
    if (level > 0 && end < line.size() && line[end] == ' ')
        ++end;
    if (markerLength)
        *markerLength = end;
    return level;
}
QString withoutQuoteMarkers(const QString &body) {
    auto lines = body.split('\n');
    for (auto &line : lines) {
        int length = 0;
        if (quoteLevel(line, &length))
            line = line.mid(length);
    }
    return lines.join('\n');
}
bool hasQuoting(const QString &body) {
    for (const auto &line : body.split('\n'))
        if (quoteLevel(line) > 0 || isPyBitmessageSeparator(line))
            return true;
    return false;
}
QString normalizeQuotes(const QString &body) {
    const auto lines = body.split('\n');
    QStringList out;
    int depth = 0;
    for (const auto &line : lines) {
        if (isPyBitmessageSeparator(line)) {
            // The blank lines PyBitmessage leaves above the separator belong
            // to neither letter.
            const auto blank = [](const QString &line) {
                int length = 0;
                quoteLevel(line, &length);
                return line.mid(length).trimmed().isEmpty();
            };
            while (!out.isEmpty() && blank(out.last()))
                out.removeLast();
            ++depth;
            continue;
        }
        if (depth == 0) {
            out << line;
        } else {
            const QString markers(depth, '>');
            out << (line.isEmpty() ? markers
                    : quoteLevel(line) > 0 ? markers + line
                                           : markers + ' ' + line);
        }
    }
    return out.join('\n');
}
QString quoteForReply(const QString &body, const QString &attribution) {
    auto lines = normalizeQuotes(body).split('\n');
    // Drop the sender's own signature: from a "-- " line at the letter's own
    // level up to where quoted history resumes.
    for (int i = 0; i < lines.size(); ++i) {
        if (quoteLevel(lines[i]) == 0 && (lines[i] == "-- " || lines[i] == "--")) {
            int end = i;
            while (end < lines.size() && quoteLevel(lines[end]) == 0)
                ++end;
            lines.erase(lines.begin() + i, lines.begin() + end);
            break;
        }
    }
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    QStringList out{attribution};
    for (const auto &line : lines)
        out << (line.trimmed().isEmpty() ? QString(">")
                : quoteLevel(line) > 0   ? '>' + line
                                         : "> " + line);
    return out.join('\n');
}
QString withQuoteLineBreaks(const QString &markdown) {
    auto lines = markdown.split('\n');
    for (int i = 0; i + 1 < lines.size(); ++i) {
        int length = 0;
        const int level = quoteLevel(lines[i], &length);
        int nextLength = 0;
        const int next = quoteLevel(lines[i + 1], &nextLength);
        const bool hasText = lines[i].size() > length && !lines[i].trimmed().isEmpty();
        const bool nextHasText = lines[i + 1].size() > nextLength;
        if (level > 0 && next == level && hasText && nextHasText && !lines[i].endsWith("  "))
            lines[i] += "  ";
    }
    return lines.join('\n');
}
QString tightenQuotes(const QString &markdown) {
    const auto lines = markdown.split('\n');
    QStringList out;
    for (int i = 0; i < lines.size(); ++i) {
        int length = 0;
        const int level = quoteLevel(lines[i], &length);
        const bool markersOnly = level > 0 && lines[i].mid(length).trimmed().isEmpty();
        if (markersOnly && !out.isEmpty() && i + 1 < lines.size()) {
            int previousLength = 0, nextLength = 0;
            const int previous = quoteLevel(out.last(), &previousLength);
            const int next = quoteLevel(lines[i + 1], &nextLength);
            if (previous == level && next == level && out.last().size() > previousLength &&
                lines[i + 1].size() > nextLength) {
                // A paragraph gap the export put between two quoted lines.
                if (!out.last().endsWith("  "))
                    out.last() += "  ";
                continue;
            }
        }
        out << lines[i];
    }
    return out.join('\n');
}
QString toComposerMarkdown(const QString &body) {
    auto lines = withQuoteLineBreaks(body).split('\n');
    for (int i = 0; i + 1 < lines.size(); ++i)
        if ((lines[i] == "-- " || lines[i] == "--") && !lines[i + 1].trimmed().isEmpty())
            lines[i] = "--  "; // a hard break: otherwise the next line joins it
    return lines.join('\n');
}
QString fromComposerMarkdown(const QString &markdown) {
    auto lines = tightenQuotes(markdown).split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed() == "\\--") {
            // The export puts a paragraph gap between the delimiter and the
            // signature text.
            lines[i] = "-- ";
            if (i + 2 < lines.size() && lines[i + 1].isEmpty() && !lines[i + 2].trimmed().isEmpty())
                lines.removeAt(i + 1);
        } else if (lines[i].startsWith("\\-- ")) {
            // Older drafts, where the signature was merged onto one line.
            const auto rest = lines[i].mid(4);
            lines[i] = "-- ";
            lines.insert(i + 1, rest);
        }
    }
    return lines.join('\n');
}
} // namespace bm
