#include "letter_document.h"
#include "quoting.h"
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextList>

namespace bm {
namespace {
constexpr QChar kLineBreak(0x2028); // a line break inside one paragraph
const auto kFeatures = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                        QTextDocument::MarkdownNoHTML);

bool isFence(const QString &line) {
    const auto t = line.trimmed();
    return t.startsWith("```") || t.startsWith("~~~");
}
// Lines that start something of their own rather than continue a paragraph.
bool startsBlock(const QString &line) {
    // List markers at any indent (nested items); the rest at up to 3 spaces.
    static const QRegularExpression block(
        "^(\\s*([-*+]\\s|\\d+[.)]\\s)| {0,3}(#{1,6}(\\s|$)|>|```|~~~|\\||[-=_*]{3,}\\s*$))");
    return line.trimmed().isEmpty() || block.match(line).hasMatch();
}
bool isHeading(const QString &line) {
    static const QRegularExpression heading("^ {0,3}#{1,6}(\\s|$)");
    return heading.match(line).hasMatch();
}
// Email-style line breaks: every newline inside a paragraph (or list item)
// becomes a U+2028 line break, which Qt's Markdown import keeps.
QString withLineBreaks(const QString &markdown) {
    const auto lines = markdown.split('\n');
    QStringList out;
    bool fenced = false;
    for (int i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        if (isFence(line))
            fenced = !fenced;
        const bool joinNext = !fenced && !isFence(line) && !line.trimmed().isEmpty() &&
                              !isHeading(line) && i + 1 < lines.size() &&
                              !startsBlock(lines[i + 1]);
        if (!out.isEmpty() && out.last().endsWith(kLineBreak))
            out.last() += line;
        else
            out << line;
        if (joinNext)
            out.last() += kLineBreak;
    }
    return out.join('\n');
}
// One run of lines at a single quote level, markers already removed.
void insertRun(QTextDocument *doc, QTextCursor &cursor, bool &first, int level,
               const QString &text, bool markdown) {
    if (text.trimmed().isEmpty())
        return;
    if (!first) {
        cursor.movePosition(QTextCursor::End);
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    first = false;
    const int start = cursor.block().blockNumber();
    if (markdown) {
        QTextDocument part;
        part.setMarkdown(withLineBreaks(text), kFeatures);
        const auto head = part.begin();
        const auto headFormat = head.blockFormat();
        const bool headInList = head.textList() != nullptr;
        const auto headList = headInList ? head.textList()->format() : QTextListFormat();
        const bool listContinues = head.textList() && head.next().isValid() &&
                                   head.next().textList() == head.textList();
        cursor.insertFragment(QTextDocumentFragment(&part));
        // The first parsed block merges into the (empty) block at the cursor
        // and takes its format; give it back its own, list included.
        auto startBlock = doc->findBlockByNumber(start);
        QTextCursor(startBlock).setBlockFormat(headFormat);
        if (headInList) {
            if (listContinues && startBlock.next().textList())
                startBlock.next().textList()->add(startBlock);
            else
                QTextCursor(startBlock).createList(headList);
        }
    } else {
        // Plain text: paragraphs at blank lines, lines kept as line breaks.
        QStringList paragraph;
        bool firstParagraph = true;
        auto flush = [&] {
            if (paragraph.isEmpty())
                return;
            if (!firstParagraph)
                cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
            cursor.insertText(paragraph.join(kLineBreak));
            paragraph.clear();
            firstParagraph = false;
        };
        for (const auto &line : text.split('\n')) {
            if (line.trimmed().isEmpty())
                flush();
            else
                paragraph << line;
        }
        flush();
    }
    for (auto block = doc->findBlockByNumber(start); block.isValid(); block = block.next())
        setQuoteLevel(block, level);
}
bool isCodeBlock(const QTextBlock &block) {
    const auto format = block.blockFormat();
    return format.hasProperty(QTextFormat::BlockCodeFence) ||
           format.hasProperty(QTextFormat::BlockCodeLanguage);
}
bool isRule(const QTextBlock &block) {
    return block.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
}
// "--" alone, or "--" then a line break: the "-- " signature delimiter.
bool isSignatureDelimiter(const QTextBlock &block) {
    const auto text = block.text();
    return blockQuoteLevel(block) == 0 &&
           (text.trimmed() == "--" || text.startsWith("--" + QString(kLineBreak)) ||
            text.startsWith("-- " + QString(kLineBreak)));
}
// Inline formatting as Markdown. Spaces at a fragment's edges stay outside
// the markers ("** bold **" would not parse).
QString inlineMarkdown(const QTextBlock &block, const QString &continuation) {
    const bool heading = block.blockFormat().headingLevel() > 0;
    QString out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        auto text = fragment.text();
        const auto format = fragment.charFormat();
        const int lead = text.size() - QString(text).remove(QRegularExpression("^\\s+")).size();
        const int trail = text.size() - QString(text).remove(QRegularExpression("\\s+$")).size();
        auto core = text.mid(lead, text.size() - lead - trail);
        if (!core.isEmpty()) {
            if (format.fontFixedPitch() || format.fontFamilies().toStringList().contains("monospace"))
                core = '`' + core + '`';
            if (format.fontStrikeOut())
                core = "~~" + core + "~~";
            if (format.fontItalic())
                core = '*' + core + '*';
            if (!heading && format.fontWeight() >= QFont::Bold)
                core = "**" + core + "**";
            if (format.isAnchor() && !format.anchorHref().isEmpty())
                core = '[' + core + "](" + format.anchorHref() + ')';
        }
        out += text.left(lead) + core + text.right(trail);
    }
    return out.replace(kLineBreak, '\n' + continuation);
}
} // namespace

QColor quoteColor(int level) {
    static const QColor colors[] = {"#4a8fd6", "#3fa46a", "#c98a1e", "#9a63c9"};
    return colors[(qMax(level, 1) - 1) % 4];
}
int blockQuoteLevel(const QTextBlock &block) {
    return block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
}
void setQuoteLevel(QTextBlock block, int level) {
    auto format = block.blockFormat();
    format.setProperty(QTextFormat::BlockQuoteLevel, level);
    format.setLeftMargin(level * kQuoteIndent + (level ? 6 : 0));
    if (level) {
        auto tint = quoteColor(level);
        tint.setAlpha(22);
        format.setBackground(tint);
    } else {
        format.clearBackground();
    }
    QTextCursor(block).setBlockFormat(format);
}
void loadLetter(QTextDocument *doc, const QString &body, bool markdown) {
    doc->clear();
    QTextCursor cursor(doc);
    bool first = true;
    QStringList run;
    int runLevel = -1;
    auto flush = [&] {
        if (runLevel >= 0)
            insertRun(doc, cursor, first, runLevel, run.join('\n'), markdown);
        run.clear();
    };
    for (const auto &line : normalizeQuotes(body).split('\n')) {
        int length = 0;
        const int level = quoteLevel(line, &length);
        // A blank line between two quoted runs belongs to neither.
        if (level == 0 && line.trimmed().isEmpty() && runLevel > 0) {
            run << QString();
            continue;
        }
        if (level != runLevel) {
            flush();
            runLevel = level;
        }
        run << line.mid(length);
    }
    flush();
    doc->clearUndoRedoStacks();
}
QString letterMarkdown(const QTextDocument *doc) {
    QStringList out;
    QTextBlock previous;
    bool fenced = false;
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        const int level = blockQuoteLevel(block);
        const QString quote = level ? QString(level, '>') + ' ' : QString();
        const bool code = isCodeBlock(block);
        if (fenced && !code) {
            out << QString(blockQuoteLevel(previous), '>') + (blockQuoteLevel(previous) ? " " : "") +
                       "```";
            fenced = false;
        }
        if (previous.isValid() && !(code && fenced)) {
            // List items follow each other tightly, nested levels included
            // (each nesting level is its own Qt list).
            const auto list = block.textList(), previousList = previous.textList();
            const bool sameList = list && previousList &&
                                  (list == previousList ||
                                   list->format().indent() != previousList->format().indent());
            // A bare "--" block (older drafts) runs straight into its signature
            // text; stepping into a deeper quote needs no gap ("text\n> quote"),
            // but stepping back out does, or the text would join the quote.
            const bool bareDelimiter = previous.text().trimmed() == "--" &&
                                       blockQuoteLevel(previous) == 0;
            if (!sameList && !bareDelimiter && level <= blockQuoteLevel(previous)) {
                const int shared = qMin(level, blockQuoteLevel(previous));
                out << QString(shared, '>');
            }
        }
        if (code) {
            if (!fenced) {
                out << quote + "```" + block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
                fenced = true;
            }
            for (const auto &line : block.text().split(kLineBreak))
                out << quote + line;
        } else if (isRule(block)) {
            out << quote + "---";
        } else if (isSignatureDelimiter(block)) {
            auto text = block.text();
            text.replace(QRegularExpression("^-- ?"), "-- ");
            if (text.trimmed() == "--")
                text = "-- ";
            out << text.replace(kLineBreak, '\n');
        } else {
            QString prefix = quote;
            QString continuation = quote;
            if (const int heading = block.blockFormat().headingLevel()) {
                prefix += QString(heading, '#') + ' ';
            } else if (auto list = block.textList()) {
                const auto indent = QString((qMax(list->format().indent(), 1) - 1) * 4, ' ');
                const auto marker =
                    list->format().style() == QTextListFormat::ListDecimal
                        ? QString::number(list->itemNumber(block) + 1) + ". "
                        : QString("- ");
                prefix += indent + marker;
                continuation += indent + QString(marker.size(), ' ');
            }
            auto text = inlineMarkdown(block, continuation);
            // A paragraph that happens to start like Markdown syntax.
            static const QRegularExpression looksLikeSyntax("^(#{1,6}\\s|>|[-*+]\\s|\\d+[.)]\\s)");
            if (prefix == quote && looksLikeSyntax.match(text).hasMatch())
                text.prepend('\\');
            out << prefix + text;
        }
        previous = block;
    }
    if (fenced)
        out << QString(blockQuoteLevel(previous), '>') + (blockQuoteLevel(previous) ? " " : "") + "```";
    while (!out.isEmpty() && out.last().trimmed().isEmpty())
        out.removeLast();
    return out.join('\n');
}
} // namespace bm
