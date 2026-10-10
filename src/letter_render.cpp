#include "letter_render.h"
#include "quoting.h"
#include "letter_document.h"
#include <QCryptographicHash>
#include <QtWidgets>

namespace bm {
void AddressHighlighter::highlightBlock(const QString &text) {
    // Raw ">" quoting (the Plain view) in its level's colour.
    if (const int level = quoteLevel(text)) {
        QTextCharFormat quoted;
        quoted.setForeground(quoteColor(level));
        setFormat(0, text.size(), quoted);
    }
    static const QRegularExpression address("\\bBM-[1-9A-HJ-NP-Za-km-z]{20,50}\\b");
    QTextCharFormat format;
    format.setFontFamilies(addressFont().families());
    auto matches = address.globalMatch(text);
    while (matches.hasNext()) {
        auto match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), format);
    }
}
// Some objects that decrypt for us are not text. In the wild (the public
// Bitmessage chan, measured over 424 posts) the garbage is mostly uniformly
// random *printable* ASCII with the odd control byte -- only ~2% control
// characters, so counting unprintables alone misses it. What gives it away
// is structure: random ASCII has almost no whitespace (under 5% there,
// against 9% and up for every real letter) and dense punctuation (~30%).
// Symbols alone are not enough: real letters with "-----" signature lines
// reach 55% -- so a symbol that merely repeats the one before it (a rule
// line, "!!!") doesn't count; random bytes almost never repeat. Truly binary
// bodies still trip the first test.
bool looksCryptic(const QString &text) {
    int unprintable = 0, space = 0, symbol = 0;
    QChar previous;
    for (const QChar c : text) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
            ++space;
        else if (c == QChar::ReplacementCharacter || c.category() == QChar::Other_Control)
            ++unprintable;
        else if (!c.isLetterOrNumber() && c != previous)
            ++symbol;
        previous = c;
    }
    const auto n = text.size();
    if (n == 0)
        return false;
    if (unprintable * 20 > n)
        return true;
    return n >= 24 && space * 100 < n * 8 && symbol * 100 > n * 25;
}
bool looksCryptic(const QString &subject, const QString &body) {
    return looksCryptic(subject) || looksCryptic(body);
}
QString crypticLabel(const QString &hash) {
    return "<cryptic-" + hash.left(6) + ">";
}
// Written character by character into a string sized up front: a big letter's
// dump runs to a million characters, and building it with QString::arg took
// a hundred times longer.
QString hexDump(const QByteArray &bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    QString out((bytes.size() + 15) / 16 * 79, Qt::Uninitialized);
    auto o = out.data();
    for (qsizetype offset = 0; offset < bytes.size(); offset += 16) {
        for (int shift = 28; shift >= 0; shift -= 4)
            *o++ = QLatin1Char(digits[(offset >> shift) & 0xf]);
        *o++ = u' ';
        *o++ = u' ';
        const auto count = qMin<qsizetype>(16, bytes.size() - offset);
        for (int i = 0; i < 16; ++i) {
            if (i < count) {
                const auto c = quint8(bytes[offset + i]);
                *o++ = QLatin1Char(digits[c >> 4]);
                *o++ = QLatin1Char(digits[c & 0xf]);
            } else {
                *o++ = u' ';
                *o++ = u' ';
            }
            *o++ = u' ';
            if (i == 7)
                *o++ = u' ';
        }
        *o++ = u' ';
        *o++ = u'|';
        for (qsizetype i = 0; i < count; ++i) {
            const auto c = quint8(bytes[offset + i]);
            *o++ = c >= 0x20 && c < 0x7f ? QChar(c) : QChar(u'.');
        }
        *o++ = u'|';
        *o++ = u'\n';
    }
    out.truncate(o - out.data());
    return out;
}
QString singleLine(QString text) {
    return text.replace('\n', ' ').replace('\r', ' ');
}
// Perceived lightness runs very differently across the hue wheel -- a raw
// 52% yellow glares while the same blue reads muddy -- so the fixed lightness
// is nudged per hue the way jdenticon does it.
QColor identiconColor(double hue) {
    static const double correctors[] = {0.55, 0.5, 0.5, 0.46, 0.6, 0.55, 0.55};
    const double corrector = correctors[int(hue * 6 + 0.5)];
    const double lightness = corrector + (0.52 - 0.5) * (1 - corrector) * 2;
    return QColor::fromHslF(hue, 0.55, qBound(0.0, lightness, 1.0));
}
// A GitHub-style identicon: a 5x5 grid mirrored left to right, one colour per
// address, on a transparent ground.
//
// Seeded from the address alone -- deliberately no per-install salt. The point
// of showing a mark beside a letter is that you can recognise the sender's
// address across clients and machines, which a salt (PyBitmessage's
// "identiconsuffix", say) destroys by making every install draw the same
// address differently.
//
// Cell edges are computed by integer division of the full size so neighbouring
// cells always share a boundary exactly: no seams, no overlap, any size.
QPixmap identiconPixmap(const QString &address, int size) {
    const auto digest = QCryptographicHash::hash(address.toUtf8(), QCryptographicHash::Md5);
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setPen(Qt::NoPen);
    p.setBrush(identiconColor(((quint8(digest[0]) << 8) | quint8(digest[1])) / 65536.0));
    const auto edge = [size](int i) { return i * size / 5; };
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 5; ++y) {
            if (quint8(digest[x * 5 + y]) % 2)
                continue;
            const QRect cell(edge(x), edge(y), edge(x + 1) - edge(x), edge(y + 1) - edge(y));
            p.drawRect(cell);
            if (x < 2)
                p.drawRect(cell.translated(edge(4 - x) - edge(x), 0));
        }
    p.end();
    return pixmap;
}
// The bars, painted over the viewport of a text view after its text.
void paintQuoteBars(QTextEdit *edit) {
    QPainter p(edit->viewport());
    const auto doc = edit->document();
    const auto layout = doc->documentLayout();
    const QPointF offset(-edit->horizontalScrollBar()->value(),
                         -edit->verticalScrollBar()->value());
    const auto visible = QRectF(edit->viewport()->rect());
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        const int level = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
        if (!level)
            continue;
        const auto rect = layout->blockBoundingRect(block).translated(offset);
        const auto next = block.next();
        const int nextLevel =
            next.isValid() ? next.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) : 0;
        const qreal nextTop =
            next.isValid() ? layout->blockBoundingRect(next).translated(offset).top() : rect.bottom();
        if (!QRectF(rect.topLeft(), QPointF(rect.right(), nextTop)).intersects(visible))
            continue;
        // A bar runs on through the gap to the next block while the quote at
        // its level continues there, so a quote reads as one strip.
        for (int i = 1; i <= level; ++i)
            p.fillRect(QRectF(doc->documentMargin() + offset.x() + (i - 1) * kQuoteIndent + 2,
                              rect.top(), 3,
                              (nextLevel >= i ? qMax(nextTop, rect.bottom()) : rect.bottom()) -
                                  rect.top()),
                       quoteColor(i));
    }
}
namespace {
constexpr int kWidestPicture = 640, kNarrowestPicture = 160;
const QString kPictureScheme = QStringLiteral("ynotbit-picture:");
// Pictures as shown, shared by every letter view: once they would take more
// than this (KiB), the least recently shown go first.
constexpr int kShownPicturesKiB = 64 * 1024;
QCache<QString, QPixmap> &shownPictures() {
    static QCache<QString, QPixmap> cache(kShownPicturesKiB);
    return cache;
}
int decodes = 0;
// What a letter shows for a picture it does not load (remote, local, or one
// that will not decode). Never nothing: given no picture, Qt goes and loads
// the name as a local file.
QPixmap placeholder() {
    const qreal ratio = qGuiApp->devicePixelRatio();
    const auto key = QString("placeholder@%1").arg(ratio);
    if (const auto cached = shownPictures().object(key))
        return *cached;
    const auto pixmap = materialIcon("image", QColor("#8a96a0")).pixmap(QSize(16, 16), ratio);
    shownPictures().insert(key, new QPixmap(pixmap), 1);
    return pixmap;
}
// As large as the picture, but no wider than room.
QSize shownSize(QSize size, int room) {
    if (size.width() <= room)
        return size;
    return {room, qMax(1, qRound(size.height() * qreal(room) / size.width()))};
}
} // namespace
int pictureDecodes() {
    return decodes;
}
int SafeDocument::pictureRoom() const {
    // As wide as the text at most, and never wider than a comfortable column.
    const int room = textWidth() > 0 ? int(textWidth() - 2 * documentMargin()) : kWidestPicture;
    return qBound(kNarrowestPicture, room, kWidestPicture);
}
const SafeDocument::Picture *SafeDocument::picture(const QString &name, QString *key) {
    if (name.startsWith(kPictureScheme)) {
        *key = name.mid(kPictureScheme.size()).section('/', 0, 0);
    } else if (name.startsWith("data:", Qt::CaseInsensitive)) {
        auto known = keys_.constFind(name);
        if (known == keys_.cend()) {
            QString found;
            if (const auto size = letterImageSize(QUrl(name)); size.isValid()) {
                found = QString::fromLatin1(
                    QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex());
                pictures_.insert(found, {name, size});
            }
            known = keys_.insert(name, found);
        }
        *key = *known;
    } else {
        return nullptr;
    }
    const auto it = pictures_.constFind(*key);
    return it == pictures_.cend() ? nullptr : &*it;
}
void SafeDocument::fitPictures() {
    const int room = pictureRoom();
    struct Change {
        int position, length;
        QTextImageFormat format;
    };
    QList<Change> changes;
    for (auto block = begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            auto format = fragment.charFormat().toImageFormat();
            QString key;
            const auto shown = picture(format.name(), &key);
            if (!shown)
                continue;
            const auto size = shownSize(shown->size, room);
            const auto name = kPictureScheme + key + '/' + QString::number(size.width());
            if (format.name() == name && qRound(format.width()) == size.width() &&
                qRound(format.height()) == size.height())
                continue;
            format.setName(name);
            format.setWidth(size.width());
            format.setHeight(size.height());
            changes << Change{fragment.position(), fragment.length(), format};
        }
    if (changes.isEmpty())
        return;
    // A read-only letter: nothing to undo.
    const bool undo = isUndoRedoEnabled();
    setUndoRedoEnabled(false);
    QTextCursor cursor(this);
    cursor.beginEditBlock();
    for (const auto &change : changes) {
        cursor.setPosition(change.position);
        cursor.setPosition(change.position + change.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(change.format);
    }
    cursor.endEditBlock();
    setUndoRedoEnabled(undo);
}
QVariant SafeDocument::loadResource(int type, const QUrl &name) {
    QString key;
    const auto text = name.toString();
    const auto shown = type == QTextDocument::ImageResource ? picture(text, &key) : nullptr;
    if (!shown)
        return QVariant::fromValue(type == QTextDocument::ImageResource ? placeholder()
                                                                        : QPixmap());
    // The size a short name gives; for a data: URL, what the text width allows.
    const int width =
        text.startsWith(kPictureScheme) ? text.section('/', 1).toInt() : pictureRoom();
    const auto size = shownSize(shown->size, width);
    // Drawn pixel for pixel: as many pixels as the screen has for its points.
    const qreal ratio = qGuiApp->devicePixelRatio();
    const auto cacheKey =
        QString("%1/%2x%3@%4").arg(key).arg(size.width()).arg(size.height()).arg(ratio);
    if (const auto cached = shownPictures().object(cacheKey))
        return QVariant::fromValue(cached->isNull() ? placeholder() : *cached);
    ++decodes;
    auto image = letterImage(QUrl(shown->url));
    const QSize pixels = (QSizeF(size) * ratio).toSize();
    if (!image.isNull() && image.size() != pixels)
        image = image.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    auto pixmap = QPixmap::fromImage(std::move(image));
    pixmap.setDevicePixelRatio(ratio);
    // Kept even when it would not decode, so it is not tried at every paint.
    shownPictures().insert(cacheKey, new QPixmap(pixmap),
                           qMax(1, int(qint64(pixmap.width()) * pixmap.height() * 4 / 1024)));
    return QVariant::fromValue(pixmap.isNull() ? placeholder() : pixmap);
}
// Markdown with quoting, through loadLetter: each quote level is read on its
// own, so quoted headings, lists and emphasis keep their quote.
void renderMarkdown(QTextBrowser *body, const QString &text) {
    auto doc = body->document();
    doc->setLayoutEnabled(false);
    loadLetter(doc, text, true);
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        QTextCursor blockCursor(block);
        auto format = block.blockFormat();
        // Airy lines for text; a picture's line is as tall as the picture.
        if (!block.text().contains(QChar::ObjectReplacementCharacter))
            format.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
        format.setBottomMargin(block.textList() ? 3 : 10);
        blockCursor.setBlockFormat(format);
    }
    if (auto safe = dynamic_cast<SafeDocument *>(doc))
        safe->fitPictures();
    doc->setLayoutEnabled(true);
    body->moveCursor(QTextCursor::Start);
    body->verticalScrollBar()->setValue(0);
}
// Headings, paired **bold**/__bold__, `code`, and [text](url) links are
// specific enough that even one match is enough, and so is ynotbit's own
// signature line: those letters come from its Markdown composer. Dash and
// numbered lists are not counted at all: plain-text letters use them all the time.
bool looksLikeMarkdown(const QString &source) {
    // Quoted Markdown counts too: a plain answer to a Markdown letter.
    const auto text = withoutQuoteMarkers(source);
    static const QRegularExpression heading("^#{1,6}[ \\t]+\\S.*$",
                                            QRegularExpression::MultilineOption);
    static const QRegularExpression bold("\\*\\*[^*\\n]+\\*\\*|__[^_\\n]+__");
    static const QRegularExpression code("`[^`\\n]+`");
    static const QRegularExpression link("\\[[^\\]\\n]+\\]\\([^)\\n]+\\)");
    // A picture by reference, ![alt][img1], or PyBitmessage's <img src="data:...">.
    static const QRegularExpression image("!\\[[^\\]\\n]*\\]\\[[^\\]\\n]+\\]|<img\\b[^>]*\\bsrc\\s*=\\s*[\"']data:image/",
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression signature("^" + QRegularExpression::escape(kSignature) + "$",
                                              QRegularExpression::MultilineOption);
    return heading.match(text).hasMatch() || bold.match(text).hasMatch() ||
           code.match(text).hasMatch() || link.match(text).hasMatch() ||
           image.match(text).hasMatch() || signature.match(text).hasMatch();
}
BodyView detectBodyView(const QString &subject, const QString &text) {
    if (looksCryptic(subject, text))
        return BodyView::Hex;
    if (looksLikeMarkdown(text))
        return BodyView::Markdown;
    // Quoted replies read best with their quote bars.
    if (hasQuoting(text))
        return BodyView::Text;
    return BodyView::Plain;
}
void renderBody(QTextBrowser *body, const QString &text, BodyView mode) {
    // Out with the old text first: the font and wrapping below would lay it all
    // out again, and the reader's body is as tall as its text.
    body->setPlainText({});
    auto font = mode == BodyView::Text || mode == BodyView::Markdown ? QApplication::font()
                                                                     : addressFont();
    // Hex digits and dots need no shaping, and going without halves the layout
    // of a big dump.
    if (mode == BodyView::Hex)
        font.setStyleStrategy(QFont::PreferNoShaping);
    body->setFont(font);
    // A hex dump's columns only line up if the lines are left alone; everything
    // else wraps to the pane.
    body->setLineWrapMode(mode == BodyView::Hex ? QTextEdit::NoWrap : QTextEdit::WidgetWidth);
    if (mode == BodyView::Markdown) {
        renderMarkdown(body, text);
        return;
    }
    // Text shows quoting as bars (PyBitmessage's dash-separated history too);
    // Plain keeps the raw ">" lines, coloured by level, and Hex the bytes.
    if (mode == BodyView::Text)
        loadLetter(body->document(), text, false);
    else
        body->setPlainText(mode == BodyView::Hex ? hexDump(text.toUtf8()) : text);
    body->moveCursor(QTextCursor::Start);
    body->verticalScrollBar()->setValue(0);
}
// The subject is a read-only text area like the body, so any part of it can be
// selected and copied. Four lines tall; longer subjects scroll.
QTextEdit *subjectArea(QBoxLayout *layout, const QString &name) {
    auto subject = new QTextEdit;
    subject->setObjectName(name);
    subject->setReadOnly(true);
    subject->setAcceptRichText(false);
    subject->setFrameShape(QFrame::NoFrame);
    subject->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    subject->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    layout->addWidget(subject);
    return subject;
}
// Bold headline type normally; in hex mode, exactly the body's dump (same
// font, same offset / bytes / text columns) and not bold.
void setSubject(QTextEdit *subject, const QString &text, BodyView mode) {
    QFont font;
    int pixels = 13; // the window-wide QSS size, which the body gets too
    if (mode == BodyView::Hex) {
        font = addressFont();
    } else {
        font = mode == BodyView::Plain || text.contains("BM-") ? addressFont()
                                                                : QApplication::font();
        font.setWeight(QFont::DemiBold);
        pixels = 15;
    }
    // The window's "QWidget{font-size}" rule beats setFont(), so the size has
    // to be set as a style rule of the subject's own as well.
    const auto rule = QString("QTextEdit{font-size:%1px;}").arg(pixels);
    if (subject->styleSheet() != rule)
        subject->setStyleSheet(rule);
    font.setPixelSize(pixels);
    subject->setFont(font);
    subject->setLineWrapMode(mode == BodyView::Hex ? QTextEdit::NoWrap : QTextEdit::WidgetWidth);
    subject->setPlainText(mode == BodyView::Hex ? hexDump(text.toUtf8()) : text);
    subject->moveCursor(QTextCursor::Start);
    subject->setFixedHeight(QFontMetrics(font).lineSpacing() * 4 +
                            int(2 * subject->document()->documentMargin()) +
                            2 * subject->frameWidth());
}
QColor iconColor(bool dark) {
    return dark ? QColor("#c1cdd7") : QColor("#435867");
}
QIcon materialIcon(const QString &name, QColor color) {
    const int size = 80;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.scale(2.0, 2.0); // drawing coordinates below are authored for a 40x40 canvas
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    // A pencil outlined from its tip to its end, with the band at the end.
    const auto pencil = [&p](QPointF tip, QPointF end, double halfWidth) {
        const QPointF along = (end - tip) / QLineF(tip, end).length();
        const QPointF across(-along.y() * halfWidth, along.x() * halfWidth);
        const QPointF body = tip + along * (halfWidth * 1.8);
        QPainterPath outline;
        outline.moveTo(tip);
        outline.lineTo(body + across);
        outline.lineTo(end + across);
        outline.lineTo(end - across);
        outline.lineTo(body - across);
        outline.closeSubpath();
        p.drawPath(outline);
        const QPointF band = end - along * (halfWidth * 1.5);
        p.drawLine(band + across, band - across);
    };
    if (name == "reply") {
        QPainterPath path;
        path.moveTo(28, 13);
        path.cubicTo(28, 23, 19, 26, 12, 26);
        p.drawPath(path);
        QPainterPath arrow;
        arrow.moveTo(18, 19);
        arrow.lineTo(11, 26);
        arrow.lineTo(18, 33);
        p.drawPath(arrow);
    } else if (name == "forward") {
        QPainterPath path;
        path.moveTo(12, 13);
        path.cubicTo(12, 23, 21, 26, 28, 26);
        p.drawPath(path);
        QPainterPath arrow;
        arrow.moveTo(22, 19);
        arrow.lineTo(29, 26);
        arrow.lineTo(22, 33);
        p.drawPath(arrow);
    } else if (name == "archive") {
        p.drawRoundedRect(8, 9, 24, 7, 2, 2);
        p.drawRect(10, 16, 20, 15);
        p.drawLine(16, 23, 24, 23);
    } else if (name == "delete") {
        p.drawLine(10, 12, 30, 12);
        p.drawRect(16, 8, 8, 4);
        p.drawRect(12, 12, 16, 20);
        p.drawLine(18, 16, 18, 28);
        p.drawLine(22, 16, 22, 28);
    } else if (name == "edit") {
        pencil(QPointF(9, 30), QPointF(30, 9), 3.6);
    } else if (name == "rename") {
        // The pencil writing on a line of text.
        pencil(QPointF(10, 29), QPointF(28, 11), 3.2);
        p.drawLine(QPointF(14, 33.5), QPointF(32, 33.5));
    } else if (name == "compose") {
        // Envelope with a "+" badge on its bottom-right corner; the corner is
        // cleared first so the badge doesn't collide with the envelope outline.
        p.drawRoundedRect(QRectF(5, 9, 25, 18), 2.5, 2.5);
        QPainterPath flap;
        flap.moveTo(6, 10.5);
        flap.lineTo(17.5, 19);
        flap.lineTo(29, 10.5);
        p.drawPath(flap);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.drawEllipse(QPointF(30, 27), 9.5, 9.5);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(30, 21), QPointF(30, 33));
        p.drawLine(QPointF(24, 27), QPointF(36, 27));
    } else if (name == "openWindow") {
        // A box with an arrow leaving through its top-right corner.
        QPainterPath box;
        box.moveTo(18, 10);
        box.lineTo(10, 10);
        box.lineTo(10, 30);
        box.lineTo(30, 30);
        box.lineTo(30, 22);
        p.drawPath(box);
        p.drawLine(19, 21, 30, 10);
        QPainterPath head;
        head.moveTo(22, 10);
        head.lineTo(30, 10);
        head.lineTo(30, 18);
        p.drawPath(head);
    } else if (name == "restore") {
        // An undo arrow: back out of the Trash.
        QPainterPath path;
        path.moveTo(14, 16);
        path.lineTo(25, 16);
        path.cubicTo(33, 16, 33, 30, 25, 30);
        path.lineTo(15, 30);
        p.drawPath(path);
        QPainterPath head;
        head.moveTo(19, 11);
        head.lineTo(14, 16);
        head.lineTo(19, 21);
        p.drawPath(head);
    } else if (name == "deleteForever") {
        // The trash can, crossed out.
        p.drawLine(10, 12, 30, 12);
        p.drawRect(16, 8, 8, 4);
        p.drawRect(12, 12, 16, 20);
        p.drawLine(17, 18, 23, 26);
        p.drawLine(23, 18, 17, 26);
    } else if (name == "retry") {
        // A circular arrow, open at the top right.
        p.drawArc(QRectF(10, 10, 20, 20), 60 * 16, 300 * 16);
        QPainterPath head;
        head.moveTo(24.4, 6.2); // a chevron on the arc's clockwise tangent at 60 degrees
        head.lineTo(26, 12);
        head.lineTo(20.2, 13.6);
        p.drawPath(head);
    } else if (name == "cancel") {
        p.drawEllipse(QRectF(10, 10, 20, 20));
        p.drawLine(QPointF(13, 13), QPointF(27, 27));
    } else if (name == "bold") {
        p.setFont(QFont(QApplication::font().family(), 17, QFont::Bold));
        p.setPen(color);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter, "B");
    } else if (name == "italic") {
        QFont f(QApplication::font().family(), 17);
        f.setItalic(true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter, "I");
    } else if (name == "strike") {
        QFont f(QApplication::font().family(), 17);
        f.setStrikeOut(true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter, "S");
    } else if (name == "code") {
        auto f = addressFont();
        f.setPointSize(13);
        f.setBold(true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter, "</>");
    } else if (name == "link") {
        for (double angle : {-45.0, 135.0}) {
            p.save();
            p.translate(20, 20);
            p.rotate(angle);
            p.drawRoundedRect(QRectF(-11, -5, 13, 10), 5, 5);
            p.restore();
        }
    } else if (name == "image") {
        p.drawRoundedRect(8, 10, 24, 18, 2, 2);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(15, 17), 2, 2);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath mountains;
        mountains.moveTo(11, 28);
        mountains.lineTo(18, 20);
        mountains.lineTo(23, 25);
        mountains.lineTo(28, 19);
        mountains.lineTo(32, 24);
        p.setClipRect(9, 11, 22, 16);
        p.drawPath(mountains);
    } else if (name == "eraser") {
        p.drawLine(9, 30, 24, 30);
        QPainterPath eraser;
        eraser.moveTo(15, 26);
        eraser.lineTo(27, 14);
        eraser.lineTo(33, 20);
        eraser.lineTo(21, 32);
        eraser.closeSubpath();
        p.drawPath(eraser);
    } else if (name == "copy") {
        p.drawRoundedRect(14, 14, 16, 16, 3, 3);
        QPainterPath back;
        back.moveTo(10, 20);
        back.lineTo(10, 11);
        back.lineTo(11, 10);
        back.lineTo(21, 10);
        back.lineTo(22, 11);
        back.lineTo(22, 14);
        p.drawPath(back);
    } else if (name == "qr") {
        p.drawRect(8, 8, 10, 10);
        p.drawRect(22, 8, 10, 10);
        p.drawRect(8, 22, 10, 10);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (double x : {23.0, 28.5})
            for (double y : {23.0, 28.5})
                p.drawRect(QRectF(x, y, 3.5, 3.5));
    } else if (name == "filterUnread") {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(20, 20), 8, 8);
    } else if (name == "filterAnonymous") {
        p.drawRoundedRect(QRectF(6, 15, 28, 11), 5, 5);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(14, 20.5), 2.6, 2.6);
        p.drawEllipse(QPointF(26, 20.5), 2.6, 2.6);
    } else if (name == "densityComfortable") {
        for (int y : {14, 26})
            p.drawLine(6, y, 34, y);
    } else if (name == "densityCozy") {
        for (int y : {11, 20, 29})
            p.drawLine(6, y, 34, y);
    } else if (name == "densityCompact") {
        for (int y : {8, 14, 20, 26, 32})
            p.drawLine(6, y, 34, y);
    } else if (name == "viewPlain") {
        // Fixed-width cells: even dashes, evenly spaced.
        for (int y : {13, 20, 27})
            for (int x : {6, 17, 28})
                p.drawLine(x, y, x + 6, y);
    } else if (name == "viewText") {
        // Prose: ragged line endings.
        p.drawLine(6, 13, 34, 13);
        p.drawLine(6, 20, 27, 20);
        p.drawLine(6, 27, 31, 27);
    } else if (name == "viewMarkdown") {
        p.setFont(QFont(QApplication::font().family(), 15, QFont::Bold));
        p.setPen(color);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter, "M↓");
    } else if (name == "viewHex") {
        p.setFont(QFont(QApplication::font().family(), 14, QFont::Bold));
        p.setPen(color);
        p.drawText(QRectF(0, 0, 40, 40), Qt::AlignCenter, "0x");
    } else if (name == "inbox") {
        p.drawLine(8, 18, 8, 30);
        p.drawLine(8, 30, 32, 30);
        p.drawLine(32, 30, 32, 18);
        QPainterPath slot;
        slot.moveTo(8, 18);
        slot.lineTo(15, 18);
        slot.lineTo(18, 24);
        slot.lineTo(22, 24);
        slot.lineTo(25, 18);
        slot.lineTo(32, 18);
        p.drawPath(slot);
    } else if (name == "drafts") {
        QPainterPath doc;
        doc.moveTo(12, 8);
        doc.lineTo(24, 8);
        doc.lineTo(30, 14);
        doc.lineTo(30, 32);
        doc.lineTo(12, 32);
        doc.closeSubpath();
        p.drawPath(doc);
        p.drawLine(24, 8, 24, 14);
        p.drawLine(24, 14, 30, 14);
        p.drawLine(16, 21, 26, 21);
        p.drawLine(16, 26, 26, 26);
    } else if (name == "outbox") {
        p.drawLine(8, 22, 8, 30);
        p.drawLine(8, 30, 32, 30);
        p.drawLine(32, 30, 32, 22);
        p.drawLine(20, 8, 20, 23);
        QPainterPath arrow;
        arrow.moveTo(14, 14);
        arrow.lineTo(20, 8);
        arrow.lineTo(26, 14);
        p.drawPath(arrow);
    } else if (name == "sent") {
        QPainterPath plane;
        plane.moveTo(33, 7);
        plane.lineTo(16, 33);
        plane.lineTo(12, 22);
        plane.lineTo(7, 20);
        plane.closeSubpath();
        p.drawPath(plane);
        p.drawLine(33, 7, 12, 22);
    } else if (name == "channels") {
        p.drawLine(8, 15, 32, 15);
        p.drawLine(8, 25, 32, 25);
        p.drawLine(15, 6, 12, 34);
        p.drawLine(25, 6, 22, 34);
    } else if (name == "broadcasts") {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(20, 20), 2.6, 2.6);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QRectF inner(13, 13, 14, 14);
        p.drawArc(inner, -40 * 16, 80 * 16);
        p.drawArc(inner, 140 * 16, 80 * 16);
        QRectF outer(7, 7, 26, 26);
        p.drawArc(outer, -40 * 16, 80 * 16);
        p.drawArc(outer, 140 * 16, 80 * 16);
    } else if (name == "identities") {
        p.drawEllipse(QPointF(20, 14), 6, 6);
        QPainterPath body;
        body.moveTo(9, 32);
        body.cubicTo(9, 24, 15, 21, 20, 21);
        body.cubicTo(25, 21, 31, 24, 31, 32);
        p.drawPath(body);
    } else if (name == "personAdd") {
        // The identities person, shifted left, with a plus beside it.
        p.drawEllipse(QPointF(16, 14), 5.5, 5.5);
        QPainterPath body;
        body.moveTo(6, 31);
        body.cubicTo(6, 24, 11, 21, 16, 21);
        body.cubicTo(21, 21, 26, 24, 26, 31);
        p.drawPath(body);
        p.drawLine(QPointF(31, 12), QPointF(31, 22));
        p.drawLine(QPointF(26, 17), QPointF(36, 17));
    } else if (name == "contacts") {
        // An address book: a bound cover with a person on it and index tabs.
        p.drawRoundedRect(QRectF(9, 6, 21, 28), 3, 3);
        p.drawLine(QPointF(13, 6), QPointF(13, 34));
        p.drawEllipse(QPointF(21.5, 16), 3.5, 3.5);
        QPainterPath body;
        body.moveTo(16, 27);
        body.cubicTo(16, 22.5, 19, 21.5, 21.5, 21.5);
        body.cubicTo(24, 21.5, 27, 22.5, 27, 27);
        p.drawPath(body);
        p.drawLine(QPointF(30, 11), QPointF(33, 11));
        p.drawLine(QPointF(30, 18), QPointF(33, 18));
        p.drawLine(QPointF(30, 25), QPointF(33, 25));
    } else if (name == "settings") {
        // A gear: six teeth round a hole.
        const auto at = [](double degrees, double radius) {
            const double a = qDegreesToRadians(degrees);
            return QPointF(20 + radius * qCos(a), 20 + radius * qSin(a));
        };
        QPainterPath gear;
        gear.moveTo(at(-18, 10));
        for (int tooth = 0; tooth < 6; ++tooth) {
            const double middle = tooth * 60;
            gear.lineTo(at(middle - 18, 10));
            gear.lineTo(at(middle - 10, 14.5));
            gear.lineTo(at(middle + 10, 14.5));
            gear.lineTo(at(middle + 18, 10));
        }
        gear.closeSubpath();
        p.drawPath(gear);
        p.drawEllipse(QPointF(20, 20), 4.5, 4.5);
    }
    return QIcon(pixmap);
}
} // namespace bm
