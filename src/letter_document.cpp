#include "letter_document.h"
#include "quoting.h"
#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextList>
#include <QUrl>

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
// Pictures become references ("![alt][img2]"); images collects their data:
// URLs, written as definitions at the end of the letter.
QString inlineMarkdown(const QTextBlock &block, const QString &continuation,
                       QStringList &images) {
    const bool heading = block.blockFormat().headingLevel() > 0;
    QString out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        auto text = fragment.text();
        const auto format = fragment.charFormat();
        if (format.isImageFormat()) {
            const auto image = format.toImageFormat();
            const auto url = image.name();
            if (!url.startsWith("data:", Qt::CaseInsensitive))
                continue;
            auto alt = image.property(QTextFormat::ImageAltText).toString();
            static const QRegularExpression unsafe("[\\[\\]\\n\\x{2028}]");
            alt.remove(unsafe);
            if (!images.contains(url))
                images << url;
            const auto reference =
                "![" + alt + "][img" + QString::number(images.indexOf(url) + 1) + ']';
            // Identical pictures side by side share one fragment.
            for (int i = 0; i < text.size(); ++i)
                out += reference;
            continue;
        }
        int lead = 0, trail = 0;
        while (lead < text.size() && text[lead].isSpace())
            ++lead;
        while (trail < text.size() - lead && text[text.size() - 1 - trail].isSpace())
            ++trail;
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
    const auto source = markdown ? inlineImageReferences(normalizeQuotes(body))
                                 : normalizeQuotes(body);
    for (const auto &line : source.split('\n')) {
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
    QStringList out, images;
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
            auto text = inlineMarkdown(block, continuation, images);
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
    if (!images.isEmpty()) {
        out << QString();
        for (int i = 0; i < images.size(); ++i)
            out << "[img" + QString::number(i + 1) + "]: " + images[i];
    }
    return out.join('\n');
}
namespace {
const QStringList kImageTypes{"image/png", "image/jpeg", "image/jpg", "image/gif", "image/webp"};
const QList<QByteArray> kImageFormats{"png", "jpeg", "gif", "webp"};
// Comfortably more than one Bitmessage object carries.
constexpr qsizetype kMaxImageUrl = 400 * 1024;
QString referenceLabel(const QString &label) {
    return label.simplified().toCaseFolded();
}
} // namespace
QImage letterImage(const QUrl &url) {
    if (url.scheme().compare("data", Qt::CaseInsensitive) != 0)
        return {};
    const auto spec = url.path(QUrl::FullyDecoded);
    const auto comma = spec.indexOf(',');
    if (comma < 0 || spec.size() > kMaxImageUrl)
        return {};
    const auto header = spec.left(comma).toLower().split(';');
    if (!kImageTypes.contains(header.first().trimmed()) || !header.contains("base64"))
        return {};
    auto decoded = QByteArray::fromBase64Encoding(spec.mid(comma + 1).toLatin1(),
                                                  QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return {};
    QBuffer buffer(&*decoded);
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    // The declared type is only a label: what decodes is decided by the bytes,
    // and only the raster formats above (never SVG) are accepted.
    if (!reader.canRead() || !kImageFormats.contains(reader.format()))
        return {};
    const auto size = reader.size();
    if (!size.isValid() || size.width() > kMaxLetterImageSide ||
        size.height() > kMaxLetterImageSide)
        return {};
    reader.setAutoTransform(true);
    return reader.read();
}
QString imageDataUrl(const QImage &source, int budget) {
    if (source.isNull() || budget <= 0)
        return {};
    // No larger than a reading pane shows anyway.
    QSize size = source.size().scaled(1600, 1600, Qt::KeepAspectRatio).boundedTo(source.size());
    bool transparent = source.hasAlphaChannel();
    const auto fits = [budget](const QByteArray &bytes, const char *type) {
        return qsizetype(strlen(type)) + 13 + (bytes.size() + 2) / 3 * 4 <= budget;
    };
    const auto url = [](const QByteArray &bytes, const char *type) {
        return QString("data:%1;base64,%2").arg(type, QString::fromLatin1(bytes.toBase64()));
    };
    while (size.width() >= 16 && size.height() >= 16) {
        // Redrawn onto a fresh image: nothing of the original file (EXIF,
        // location, comments) comes along.
        QImage image(size, transparent ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        image.fill(transparent ? Qt::transparent : Qt::white);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRect(QPoint(), size), source);
        }
        const auto encode = [&image](const char *format, int quality) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, format, quality);
            return bytes;
        };
        // Lossless when it fits (drawings, screenshots); else a photo's JPEG.
        if (const auto png = encode("PNG", -1); fits(png, "image/png"))
            return url(png, "image/png");
        if (transparent) {
            transparent = false; // a white background, then try again as a photo
            continue;
        }
        for (int quality : {85, 75, 60, 45})
            if (const auto jpeg = encode("JPEG", quality); fits(jpeg, "image/jpeg"))
                return url(jpeg, "image/jpeg");
        size = size * 3 / 4;
    }
    return {};
}
QString inlineImageReferences(const QString &markdown) {
    static const QRegularExpression definition(
        "^ {0,3}\\[([^\\]\\n]+)\\]:[ \\t]*<?(data:image/[A-Za-z0-9.+-]+;base64,[A-Za-z0-9+/=]+)>?"
        "[ \\t]*$");
    QHash<QString, QString> urls;
    QStringList kept;
    for (const auto &line : markdown.split('\n')) {
        int length = 0;
        quoteLevel(line, &length);
        if (const auto match = definition.match(line.mid(length)); match.hasMatch()) {
            urls.insert(referenceLabel(match.captured(1)), match.captured(2));
            continue;
        }
        kept << line;
    }
    // PyBitmessage's HTML pictures: <img src="data:...">, shown only for data:
    // URLs there too (its safe HTML parser).
    static const QRegularExpression html(
        "<img\\b[^>]*?\\bsrc\\s*=\\s*[\"'](data:image/[A-Za-z0-9.+-]+;base64,[A-Za-z0-9+/=\\s]+)"
        "[\"'][^>]*>",
        QRegularExpression::CaseInsensitiveOption);
    auto text = kept.join('\n');
    for (auto match = html.match(text); match.hasMatch(); match = html.match(text, match.capturedStart() + 1)) {
        auto url = match.captured(1);
        url.remove(QRegularExpression("\\s"));
        text.replace(match.capturedStart(), match.capturedLength(), "![image](" + url + ')');
    }
    // Qt's Markdown import drops a picture with no alt text.
    static const QRegularExpression noAlt("!\\[\\]\\(");
    text.replace(noAlt, "![image](");
    if (urls.isEmpty())
        return text;
    // ![alt][label], ![alt][] and ![alt]; not ![alt](inline).
    static const QRegularExpression reference("!\\[([^\\]\\n]*)\\](?:\\[([^\\]\\n]*)\\])?(?!\\()");
    QString out;
    qsizetype done = 0;
    for (auto it = reference.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        const auto label = match.captured(2).isEmpty() ? match.captured(1) : match.captured(2);
        const auto url = urls.value(referenceLabel(label));
        if (url.isEmpty())
            continue;
        out += text.mid(done, match.capturedStart() - done);
        const auto alt = match.captured(1).isEmpty() ? QStringLiteral("image") : match.captured(1);
        out += "![" + alt + "](" + url + ')';
        done = match.capturedEnd();
    }
    return out + text.mid(done);
}
} // namespace bm
