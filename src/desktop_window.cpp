#include "desktop_window.h"
#include "session.h"
#include "qrcodegen.hpp"
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFileInfo>
#include <QSettings>
#include <QTextList>
#include <QtWidgets>

namespace bm {
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
namespace {
class AddressHighlighter : public QSyntaxHighlighter {
  public:
    explicit AddressHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}
    void highlightBlock(const QString &text) override {
        static const QRegularExpression address("\\bBM-[1-9A-HJ-NP-Za-km-z]{20,50}\\b");
        QTextCharFormat format;
        format.setFontFamilies(addressFont().families());
        auto matches = address.globalMatch(text);
        while (matches.hasNext()) {
            auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};
QString singleLine(QString text) {
    return text.replace('\n', ' ').replace('\r', ' ');
}
class SafeDocument : public QTextDocument {
  public:
    using QTextDocument::QTextDocument;
    QVariant loadResource(int, const QUrl &) override {
        return QVariant::fromValue(QImage());
    }
};
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
// A letter's trust class, the way a postal border once told you how a piece
// of mail got to you before you read a word of it: plain domestic mail had
// no border; airmail carried a red/blue diagonal stripe; special/urgent
// service used green and yellow. Here: an ordinary direct letter, a chan
// post signed by a known member, a chan post signed anonymously with the
// chan's own shared key (delivery.cpp's chanBroadcast path -- sender ==
// recipient is how that's recognized, matching MessageModel's anonymous
// filter), and a public broadcast.
enum class LetterKind { Personal, ChanPersonal, ChanAnonymous, Broadcast };
LetterKind classifyLetter(const QString &folder, const QString &from, const QString &to) {
    if (folder == "Broadcasts")
        return LetterKind::Broadcast;
    if (folder == "Channels")
        return from == to ? LetterKind::ChanAnonymous : LetterKind::ChanPersonal;
    return LetterKind::Personal;
}
struct KindColors {
    QColor a, b;
    bool diagonal;
};
// Solid accent for a chan-personal post; green/yellow diagonal for broadcast
// (echoing old special-delivery markings). Chan-anonymous gets a translucent
// gray diagonal rather than a loud color -- it should read as "unattributed"
// rather than "urgent", and alpha over whatever's underneath keeps it legible
// on both a dark and a light palette without a separate color per theme.
KindColors kindColors(LetterKind kind, const QPalette &palette) {
    // Every letter's border shares one faint ground -- the theme's own text
    // colour at low alpha, so it reads as a near-white watermark on a light
    // theme and the reverse on a dark one -- and only the stripes laid over
    // it say what kind of letter this is. A fixed gray would look like a
    // smudge on light themes and vanish on dark ones.
    const auto ink = palette.color(QPalette::WindowText);
    const QColor ground(ink.red(), ink.green(), ink.blue(), 18);
    switch (kind) {
    case LetterKind::Personal:
        // A direct letter (Inbox/Outbox/Sent/...). Saturated enough to still
        // read as green over a dark pane that already leans blue/teal.
        return {ground, QColor(30, 170, 60, 60), true};
    case LetterKind::ChanPersonal:
        // A chan post from a known member.
        return {ground, QColor(60, 125, 217, 60), true};
    case LetterKind::ChanAnonymous:
        // No colour at all: nobody vouches for it.
        return {ground, QColor(ink.red(), ink.green(), ink.blue(), 7), true};
    case LetterKind::Broadcast:
        return {QColor("#2e8b57"), QColor("#e0b400"), true};
    }
    return {{}, {}, false};
}
// The diagonal stripes are one continuous pattern rotated about a single
// shared origin, then revealed through whatever region is clipped in --
// so the same lines keep running unbroken wherever they cross a clip edge.
void paintDiagonalStripes(QPainter &p, QPoint origin, int span, KindColors c) {
    p.setPen(Qt::NoPen);
    p.setBrush(c.b);
    p.translate(origin);
    p.rotate(45);
    for (int x = -span; x < span; x += 8)
        p.drawRect(x, -span, 4, span * 2);
}
// Paints a full border ring around outer, bandWidth thick on all four sides,
// for a letter's kind -- used for the reader pane and message window frames.
// The diagonal pattern is drawn once from outer's own center and clipped to
// the ring, so it continues seamlessly around each corner instead of the
// four sides showing four unrelated, misaligned patterns.
void paintKindBorder(QPainter &p, QRect outer, int bandWidth, LetterKind kind,
                     const QPalette &palette) {
    if (outer.isEmpty())
        return;
    const auto c = kindColors(kind, palette);
    QRegion ring(outer);
    ring -= outer.adjusted(bandWidth, bandWidth, -bandWidth, -bandWidth);
    p.save();
    p.setClipRegion(ring);
    p.fillRect(outer, c.a);
    if (c.diagonal)
        paintDiagonalStripes(p, outer.center(), outer.width() + outer.height(), c);
    p.restore();
}
class LetterDelegate : public QStyledItemDelegate {
  public:
    LetterDelegate(QObject *parent, QString density)
        : QStyledItemDelegate(parent), density_(std::move(density)) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        if (density_ == "compact")
            return {280, 30};
        if (density_ == "cozy")
            return {280, 58};
        return {280, 94};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        p->save();
        const auto pal = o.palette;
        p->fillRect(o.rect, o.state & QStyle::State_Selected ? pal.highlight() : pal.base());
        const bool unread = i.data(Qt::UserRole + 9).toBool();
        // Drafts/Outbox/Sent are always from you -- your own identicon on every
        // row would be noise, so show who the letter is going to instead.
        const auto folder = i.data(Qt::UserRole + 6).toString();
        const bool useRecipient = folder == "Drafts" || folder == "Outbox" || folder == "Sent";
        const auto identityAddress = i.data(Qt::UserRole + (useRecipient ? 3 : 2)).toString();
        const int iconSize = density_ == "compact" ? 20 : density_ == "cozy" ? 28 : 34;
        if (!identityAddress.isEmpty()) {
            auto icon = identiconPixmap(identityAddress, iconSize);
            p->drawPixmap(o.rect.left() + 12, o.rect.top() + (o.rect.height() - iconSize) / 2, icon);
        }
        const int textLeft = 12 + iconSize + 10;
        auto text = [&](int y, QString value, bool bold, QColor color) {
            QFont font = o.font;
            if (value.contains("BM-"))
                font.setFamilies(addressFont().families());
            font.setBold(bold);
            p->setFont(font);
            p->setPen(color);
            QRect r = o.rect.adjusted(textLeft, y, -16, 0);
            r.setHeight(24);
            p->drawText(
                r, Qt::AlignVCenter,
                QFontMetrics(font).elidedText(singleLine(value), Qt::ElideRight, r.width()));
        };
        const auto preview = i.data(Qt::UserRole + 5).toString();
        const bool cryptic = looksCryptic(i.data(Qt::UserRole + 4).toString(), preview);
        const auto subject = cryptic ? crypticLabel(i.data(Qt::UserRole + 1).toString())
                                     : i.data(Qt::UserRole + 4).toString();
        const auto previewLine = cryptic ? QString() : preview;
        if (density_ == "compact") {
            text(3, subject, unread, pal.text().color());
        } else if (density_ == "cozy") {
            text(8, subject, unread, pal.text().color());
            text(32, previewLine, false, pal.placeholderText().color());
        } else {
            text(10, subject, unread, pal.text().color());
            text(36, previewLine, false, pal.placeholderText().color());
            auto state = i.data(Qt::UserRole + 7).toString();
            const auto stateColor =
                state == "acknowledged"
                    ? QColor(pal.base().color().lightness() < 128 ? "#8ce0b2" : "#17643b")
                    : pal.placeholderText().color();
            text(62,
                 state.isEmpty() ? i.data(Qt::UserRole + 11).toString() : state.replace('_', ' '),
                 false, stateColor);
        }
        p->setPen(pal.mid().color());
        p->drawLine(o.rect.bottomLeft(), o.rect.bottomRight());
        p->restore();
    }

  private:
    QString density_;
};
// The reader pane's own envelope border, echoing the selected letter's kind
// on all four edges of the message content -- subject, metadata and body sit
// inside it, the way a letter sits inside its envelope. A plain letter draws
// no border and reserves no margin for one.
class KindFrame : public QWidget {
  public:
    explicit KindFrame(QWidget *parent = nullptr) : QWidget(parent) {
        layout_ = new QVBoxLayout(this);
        layout_->setSpacing(14);
        layout_->setContentsMargins(0, 0, 0, 0);
    }
    QVBoxLayout *contentLayout() const {
        return layout_;
    }
    void setKind(LetterKind kind) {
        kind_ = kind;
        // A gap between the border and its content -- otherwise the toolbar
        // sits flush against the inner edge of the stripe with no breathing
        // room. Every kind now paints some border, so the inset is constant.
        const int inset = kBand + 3;
        layout_->setContentsMargins(inset, inset, inset, inset);
        update();
    }
  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        paintKindBorder(p, rect(), kBand, kind_, palette());
    }

  private:
    static constexpr int kBand = 5;
    QVBoxLayout *layout_;
    LetterKind kind_ = LetterKind::Personal;
};
QPushButton *button(QString text, QBoxLayout *layout, std::function<void()> fn) {
    auto b = new QPushButton(text);
    layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
    return b;
}
void renderMarkdown(QTextBrowser *body, const QString &text) {
    body->document()->setLayoutEnabled(false);
    body->document()->setMarkdown(
        text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                              QTextDocument::MarkdownNoHTML));
    body->document()->clearUndoRedoStacks();
    for (auto block = body->document()->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        format.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
        format.setBottomMargin(block.textList() ? 3 : 10);
        cursor.setBlockFormat(format);
    }
    body->document()->setLayoutEnabled(true);
    body->moveCursor(QTextCursor::Start);
    body->verticalScrollBar()->setValue(0);
}
// How a letter's body is shown. Detected per message, overridable per message
// from the reader's view switch.
enum class BodyView { Plain, Text, Markdown, Hex };
// Headings, paired **bold**/__bold__, `code`, and [text](url) links are
// specific enough that even one match is enough. A single dash line is not --
// plenty of ordinary letters start a line with a dash -- so lists need two.
bool looksLikeMarkdown(const QString &text) {
    static const QRegularExpression heading("^#{1,6}[ \\t]+\\S.*$",
                                            QRegularExpression::MultilineOption);
    static const QRegularExpression bold("\\*\\*[^*\\n]+\\*\\*|__[^_\\n]+__");
    static const QRegularExpression code("`[^`\\n]+`");
    static const QRegularExpression link("\\[[^\\]\\n]+\\]\\([^)\\n]+\\)");
    static const QRegularExpression listLine("^[ \\t]*[-*+][ \\t]+\\S.*$",
                                             QRegularExpression::MultilineOption);
    static const QRegularExpression numberedLine("^[ \\t]*\\d+\\.[ \\t]+\\S.*$",
                                                 QRegularExpression::MultilineOption);
    auto count = [&](const QRegularExpression &re) {
        int n = 0;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            it.next();
            ++n;
        }
        return n;
    };
    if (heading.match(text).hasMatch() || bold.match(text).hasMatch() ||
        code.match(text).hasMatch() || link.match(text).hasMatch())
        return true;
    return count(listLine) >= 2 || count(numberedLine) >= 2;
}
BodyView detectBodyView(const QString &subject, const QString &text) {
    if (looksCryptic(subject, text))
        return BodyView::Hex;
    if (looksLikeMarkdown(text))
        return BodyView::Markdown;
    return BodyView::Plain;
}
QString hexDump(const QByteArray &bytes) {
    QString out;
    for (int offset = 0; offset < bytes.size(); offset += 16) {
        const auto line = bytes.mid(offset, 16);
        QString hex, ascii;
        for (int i = 0; i < 16; ++i) {
            hex += i < line.size()
                       ? QString("%1 ").arg(quint8(line[i]), 2, 16, QChar('0'))
                       : QString("   ");
            if (i == 7)
                hex += ' ';
            if (i < line.size()) {
                const auto c = quint8(line[i]);
                ascii += c >= 0x20 && c < 0x7f ? QChar(c) : QChar('.');
            }
        }
        out += QString("%1  %2 |%3|\n").arg(offset, 8, 16, QChar('0')).arg(hex, ascii);
    }
    return out;
}
void renderBody(QTextBrowser *body, const QString &text, BodyView mode) {
    body->setFont(mode == BodyView::Text || mode == BodyView::Markdown ? QApplication::font()
                                                                       : addressFont());
    // A hex dump's columns only line up if the lines are left alone; everything
    // else wraps to the pane.
    body->setLineWrapMode(mode == BodyView::Hex ? QTextEdit::NoWrap : QTextEdit::WidgetWidth);
    if (mode == BodyView::Markdown) {
        renderMarkdown(body, text);
        return;
    }
    body->setPlainText(mode == BodyView::Hex ? hexDump(text.toUtf8()) : text);
    body->moveCursor(QTextCursor::Start);
    body->verticalScrollBar()->setValue(0);
}
QLabel *subjectArea(QBoxLayout *layout, const QString &labelName, const QString &scrollName) {
    auto label = new QLabel;
    label->setObjectName(labelName);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setStyleSheet("font-size:15px;font-weight:600;");
    auto scroll = new QScrollArea;
    scroll->setObjectName(scrollName);
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->setMaximumHeight(112); // 20% of the 560px initial dialog height
    layout->addWidget(scroll);
    return label;
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
        p.drawLine(11, 29, 25, 15);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(25, 15, 29, 11);
    } else if (name == "more") {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        for (int y : {10, 20, 30})
            p.drawEllipse(QPointF(20, y), 2.6, 2.6);
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
    }
    return QIcon(pixmap);
}
const QVector<QPair<QString, QString>> kFolderIcons = {
    {"Inbox", "inbox"},         {"Drafts", "drafts"},       {"Outbox", "outbox"},
    {"Sent", "sent"},           {"Channels", "channels"},   {"Broadcasts", "broadcasts"},
    {"Archive", "archive"},     {"Trash", "delete"},        {"Identities", "identities"},
};
QTextCharFormat headingCharFormat(int level) {
    QTextCharFormat t;
    t.setFontWeight(QFont::Bold);
    t.setFontPointSize(level == 1 ? 22 : level == 2 ? 18 : 15);
    return t;
}
class MarkdownEdit : public QTextEdit {
  public:
    using QTextEdit::QTextEdit;

  protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Space && tryAutoFormat())
            return;
        QTextEdit::keyPressEvent(event);
    }
    void insertFromMimeData(const QMimeData *source) override {
        if (source->hasText() && looksLikeMarkdown(source->text())) {
            insertMarkdown(source->text());
            return;
        }
        QTextEdit::insertFromMimeData(source);
    }

  private:
    void insertMarkdown(const QString &text) {
        QTextDocument doc;
        doc.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                              QTextDocument::MarkdownNoHTML));
        // Match this app's own "# "-triggered heading sizes rather than Qt's
        // markdown-parser defaults, so a pasted heading looks the same as one
        // typed by hand.
        for (auto block = doc.begin(); block.isValid(); block = block.next()) {
            int level = block.blockFormat().headingLevel();
            if (level <= 0)
                continue;
            QTextCursor c(block);
            c.setPosition(block.position());
            c.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
            c.mergeCharFormat(headingCharFormat(level));
        }
        auto cursor = textCursor();
        cursor.beginEditBlock();
        if (cursor.hasSelection())
            cursor.removeSelectedText();
        QTextCursor docCursor(&doc);
        docCursor.select(QTextCursor::Document);
        cursor.insertFragment(docCursor.selection());
        cursor.endEditBlock();
        setTextCursor(cursor);
    }
    bool tryAutoFormat() {
        auto cursor = textCursor();
        if (cursor.hasSelection())
            return false;
        auto block = cursor.block();
        auto prefix = block.text().left(cursor.position() - block.position());
        auto apply = [&](const std::function<void(QTextCursor &)> &fn) {
            cursor.beginEditBlock();
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            fn(cursor);
            cursor.endEditBlock();
            setTextCursor(cursor);
            return true;
        };
        if (prefix == "#" || prefix == "##" || prefix == "###") {
            int level = prefix.size();
            return apply([level](QTextCursor &c) {
                auto f = c.blockFormat();
                f.setHeadingLevel(level);
                c.setBlockFormat(f);
                c.mergeCharFormat(headingCharFormat(level));
            });
        }
        if (prefix == "-" || prefix == "*")
            return apply([](QTextCursor &c) {
                QTextListFormat f;
                f.setStyle(QTextListFormat::ListDisc);
                c.createList(f);
            });
        if (prefix == "1.")
            return apply([](QTextCursor &c) {
                QTextListFormat f;
                f.setStyle(QTextListFormat::ListDecimal);
                c.createList(f);
            });
        if (prefix == ">")
            return apply([](QTextCursor &c) {
                auto f = c.blockFormat();
                f.setProperty(QTextFormat::BlockQuoteLevel, 1);
                f.setLeftMargin(24);
                c.setBlockFormat(f);
            });
        return false;
    }
};
class HeadingGutter : public QWidget {
  public:
    explicit HeadingGutter(QTextEdit *editor) : editor_(editor) {
        setFixedWidth(28);
        connect(editor_->document(), &QTextDocument::contentsChanged, this, [this] { update(); });
        connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { update(); });
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        auto f = p.font();
        f.setPointSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(editor_->palette().color(QPalette::PlaceholderText));
        auto doc = editor_->document();
        auto layout = doc->documentLayout();
        int scrollOffset = editor_->verticalScrollBar()->value();
        for (auto block = doc->begin(); block.isValid(); block = block.next()) {
            int level = block.blockFormat().headingLevel();
            if (level <= 0)
                continue;
            auto rect = layout->blockBoundingRect(block);
            int top = static_cast<int>(rect.top()) - scrollOffset;
            if (top + rect.height() < 0 || top > height())
                continue;
            p.drawText(QRect(0, top, width() - 4, qMax(static_cast<int>(rect.height()), 1)),
                       Qt::AlignRight | Qt::AlignTop, "H" + QString::number(level));
        }
    }

  private:
    QTextEdit *editor_;
};
class QrCodeView : public QWidget {
  public:
    explicit QrCodeView(const QString &text, QWidget *parent = nullptr)
        : QWidget(parent), code_(qrcodegen::QrCode::encodeText(text.toUtf8().constData(),
                                                                qrcodegen::QrCode::Ecc::MEDIUM)) {
        setFixedSize(220, 220);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        int modules = code_.getSize();
        double scale = double(width()) / modules;
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        for (int y = 0; y < modules; ++y)
            for (int x = 0; x < modules; ++x)
                if (code_.getModule(x, y))
                    p.drawRect(QRectF(x * scale, y * scale, scale, scale));
    }

  private:
    qrcodegen::QrCode code_;
};
class Composer : public QDialog {
    Session &session_;
    QString id_, original_;
    QComboBox *sender_;
    QLineEdit *to_, *subject_;
    QPushButton *modePrivate_, *modePublic_;
    QWidget *floatingToolbar_;
    QTextEdit *body_;
    QLabel *status_;
    QTimer autosave_;
    bool dirty_ = false, bodyEdited_ = false;
    bool save() {
        if (!dirty_)
            return true;
        auto body = bodyEdited_ ? body_->document()->toMarkdown() : original_;
        auto id = session_.saveLetter(id_, sender_->currentData().toString(), to_->text(),
                                      subject_->text(), body,
                                      modePublic_->isChecked() ? "broadcast" : "direct");
        if (id.isEmpty()) {
            status_->setText(session_.error());
            status_->show();
            return false;
        }
        id_ = id;
        dirty_ = false;
        status_->hide();
        return true;
    }

  public:
    Composer(Session &session, QVariantMap letter, bool reply, bool dark, QWidget *parent)
        : QDialog(parent), session_(session) {
        setObjectName("composer");
        setWindowTitle(reply ? "Reply" : "Write a letter");
        resize(740, 650);
        setModal(true);
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(12);
        auto modeRow = new QHBoxLayout;
        // font-weight in the :checked rule would make the checked button's text
        // bold and wider than the sizeHint computed from its regular-weight state,
        // clipping the label -- background/border contrast alone indicates selection.
        const QString modeButtonStyle =
            "QPushButton { border: 1px solid palette(mid); }"
            "QPushButton:checked { background: palette(highlight); "
            "color: palette(highlighted-text); border-color: palette(highlight); }";
        modePrivate_ = new QPushButton;
        modePrivate_->setObjectName("modePrivateButton");
        modePrivate_->setCheckable(true);
        modePrivate_->setChecked(true);
        modePrivate_->setStyleSheet(modeButtonStyle);
        modePublic_ = new QPushButton;
        modePublic_->setObjectName("modePublicButton");
        modePublic_->setCheckable(true);
        modePublic_->setStyleSheet(modeButtonStyle);
        auto modeGroup = new QButtonGroup(this);
        modeGroup->setExclusive(true);
        modeGroup->addButton(modePrivate_);
        modeGroup->addButton(modePublic_);
        modeRow->addWidget(modePrivate_);
        modeRow->addWidget(modePublic_);
        modeRow->addStretch();
        layout->addLayout(modeRow);
        auto modeHint = new QLabel;
        modeHint->setObjectName("modeHint");
        modeHint->setWordWrap(true);
        {
            auto f = modeHint->font();
            f.setPointSizeF(f.pointSizeF() * 0.9);
            modeHint->setFont(f);
        }
        layout->addWidget(modeHint);
        sender_ = new QComboBox;
        sender_->setFont(addressFont());
        sender_->setObjectName("senderSelector");
        auto identities = session.identities();
        for (auto value : identities) {
            auto identity = value.toMap();
            sender_->addItem(identity["label"].toString(), identity["address"]);
        }
        auto from = letter[reply ? "to" : "from"].toString();
        int n = sender_->findData(from);
        if (n >= 0)
            sender_->setCurrentIndex(n);
        layout->addWidget(sender_);
        auto updateModeLabels = [this, identities, modeHint] {
            bool channel = false;
            for (auto value : identities) {
                auto identity = value.toMap();
                if (identity["address"] == sender_->currentData()) {
                    channel = identity["chan"].toBool();
                    break;
                }
            }
            modePrivate_->setText(channel ? "Personal" : "Private mail");
            modePublic_->setText(channel ? "Anonymous" : "Public mail");
            if (modePrivate_->isChecked())
                modeHint->setText(channel
                                       ? "Encrypted to the channel's shared address. Anyone who "
                                         "knows the channel phrase can read it."
                                       : "Encrypted to one recipient. Only they can read it.");
            else
                modeHint->setText(
                    channel
                        ? "Sent as the channel to everyone subscribed. Your own identity isn't "
                          "revealed."
                        : "Sent to everyone subscribed to your address. Anyone can read it.");
        };
        updateModeLabels();
        connect(sender_, &QComboBox::currentIndexChanged, this, updateModeLabels);
        connect(modePrivate_, &QPushButton::toggled, this, updateModeLabels);
        modePublic_->setChecked(!reply && letter["kind"] == "broadcast");
        modePrivate_->setChecked(reply || letter["kind"] != "broadcast");
        to_ = new QLineEdit;
        to_->setFont(addressFont());
        to_->setObjectName("recipientField");
        to_->setPlaceholderText("Recipient · BM-address");
        to_->setText(reply ? letter[letter["folder"] == "Channels" ? "to" : "from"].toString()
                           : letter["to"].toString());
        layout->addWidget(to_);
        subject_ = new QLineEdit;
        subject_->setObjectName("subjectField");
        subject_->setPlaceholderText("Subject");
        auto subject = singleLine(letter["subject"].toString());
        if (reply && !subject.startsWith("Re:", Qt::CaseInsensitive))
            subject.prepend("Re: ");
        subject_->setText(subject);
        layout->addWidget(subject_);
        auto tools = new QToolBar;
        tools->setToolButtonStyle(Qt::ToolButtonIconOnly);
        tools->setIconSize(QSize(28, 28));
        layout->addWidget(tools);
        body_ = new MarkdownEdit;
        body_->setObjectName("bodyField");
        body_->setAcceptRichText(false);
        auto doc = new SafeDocument(body_);
        body_->setDocument(doc);
        new AddressHighlighter(doc);
        original_ = reply ? QString() : letter["body"].toString();
        const bool freshLetter = reply || letter["hash"].toString().isEmpty();
        if (freshLetter)
            original_ = "-- \nsent by ynotbit";
        doc->setMarkdown(original_,
                         QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                         QTextDocument::MarkdownNoHTML));
        if (freshLetter) {
            // Leading blank lines in the markdown source get collapsed by the
            // parser, so the separator has to be inserted as real blocks instead.
            QTextCursor c(doc);
            c.insertBlock();
            c.insertBlock();
        }
        doc->clearUndoRedoStacks();
        // QTextEdit otherwise inherits the final imported fragment's format,
        // which can be an image object rather than a text insertion format.
        body_->moveCursor(QTextCursor::Start);
        body_->setCurrentCharFormat(QTextCharFormat());
        body_->setPlaceholderText("Take your time. Write something worth sending.");
        auto bodyRow = new QHBoxLayout;
        bodyRow->setContentsMargins(0, 0, 0, 0);
        bodyRow->setSpacing(4);
        auto gutter = new HeadingGutter(body_);
        gutter->setObjectName("headingGutter");
        bodyRow->addWidget(gutter);
        bodyRow->addWidget(body_, 1);
        layout->addLayout(bodyRow, 1);
        auto icon = [dark](QString name) { return materialIcon(name, iconColor(dark)); };
        auto format = [&](QString iconName, QString objName, QString tip,
                          std::function<void()> fn) {
            auto a = tools->addAction(icon(iconName), tip);
            a->setObjectName(objName);
            connect(a, &QAction::triggered, this, [this, fn] {
                fn();
                body_->setFocus();
            });
            return a;
        };
        format("bold", "boldAction", "Bold", [this] {
            QTextCharFormat f;
            f.setFontWeight(body_->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
            body_->mergeCurrentCharFormat(f);
        })->setShortcut(QKeySequence::Bold);
        format("italic", "italicAction", "Italic", [this] {
            QTextCharFormat f;
            f.setFontItalic(!body_->fontItalic());
            body_->mergeCurrentCharFormat(f);
        })->setShortcut(QKeySequence::Italic);
        format("strike", "strikeAction", "Strikethrough", [this] {
            QTextCharFormat f;
            f.setFontStrikeOut(!body_->currentCharFormat().fontStrikeOut());
            body_->mergeCurrentCharFormat(f);
        });
        format("code", "codeAction", "Inline code", [this] {
            bool isCode = body_->currentCharFormat().fontFixedPitch();
            QTextCharFormat f;
            f.setFontFixedPitch(!isCode);
            if (!isCode)
                f.setFontFamilies({"monospace"});
            body_->mergeCurrentCharFormat(f);
        });
        format("link", "linkAction", "Link", [this] {
            bool ok;
            auto url = QInputDialog::getText(this, "Insert link", "https:// address",
                                             QLineEdit::Normal, {}, &ok);
            QUrl u(url);
            if (!ok || u.scheme() != "https" || u.host().isEmpty())
                return;
            auto c = body_->textCursor();
            QTextCharFormat f;
            f.setAnchor(true);
            f.setAnchorHref(url);
            f.setFontUnderline(true);
            if (c.hasSelection())
                c.mergeCharFormat(f);
            else
                c.insertText(url, f);
        });
        format("image", "imageAction", "Images aren't supported — remote images are never "
                                       "loaded, for privacy",
               [] {})
            ->setEnabled(false);
        format("eraser", "clearFormatAction", "Clear formatting", [this] {
            auto c = body_->textCursor();
            if (c.hasSelection())
                c.setCharFormat(QTextCharFormat());
            else
                body_->setCurrentCharFormat(QTextCharFormat());
        });
        floatingToolbar_ = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint);
        floatingToolbar_->setObjectName("floatingToolbar");
        floatingToolbar_->setAttribute(Qt::WA_ShowWithoutActivating);
        auto floatLayout = new QHBoxLayout(floatingToolbar_);
        floatLayout->setContentsMargins(4, 4, 4, 4);
        floatLayout->setSpacing(2);
        auto floatButton = [&](QString iconName, QString objName, std::function<void()> fn) {
            auto b = new QToolButton;
            b->setObjectName(objName);
            b->setIcon(icon(iconName));
            b->setIconSize(QSize(20, 20));
            connect(b, &QToolButton::clicked, this, [this, fn] {
                fn();
                body_->setFocus();
            });
            floatLayout->addWidget(b);
            return b;
        };
        floatButton("bold", "floatBoldButton", [this] {
            QTextCharFormat f;
            f.setFontWeight(body_->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
            body_->mergeCurrentCharFormat(f);
        });
        floatButton("italic", "floatItalicButton", [this] {
            QTextCharFormat f;
            f.setFontItalic(!body_->fontItalic());
            body_->mergeCurrentCharFormat(f);
        });
        floatButton("strike", "floatStrikeButton", [this] {
            QTextCharFormat f;
            f.setFontStrikeOut(!body_->currentCharFormat().fontStrikeOut());
            body_->mergeCurrentCharFormat(f);
        });
        floatButton("code", "floatCodeButton", [this] {
            bool isCode = body_->currentCharFormat().fontFixedPitch();
            QTextCharFormat f;
            f.setFontFixedPitch(!isCode);
            if (!isCode)
                f.setFontFamilies({"monospace"});
            body_->mergeCurrentCharFormat(f);
        });
        floatButton("link", "floatLinkButton", [this] {
            bool ok;
            auto url = QInputDialog::getText(this, "Insert link", "https:// address",
                                             QLineEdit::Normal, {}, &ok);
            QUrl u(url);
            if (!ok || u.scheme() != "https" || u.host().isEmpty())
                return;
            auto c = body_->textCursor();
            QTextCharFormat f;
            f.setAnchor(true);
            f.setAnchorHref(url);
            f.setFontUnderline(true);
            if (c.hasSelection())
                c.mergeCharFormat(f);
            else
                c.insertText(url, f);
        });
        floatingToolbar_->hide();
        connect(body_, &QTextEdit::selectionChanged, this, [this] {
            if (!body_->textCursor().hasSelection()) {
                floatingToolbar_->hide();
                return;
            }
            auto cursor = body_->textCursor();
            cursor.setPosition(qMin(cursor.anchor(), cursor.position()));
            auto rect = body_->cursorRect(cursor);
            auto point = body_->viewport()->mapToGlobal(rect.topLeft());
            floatingToolbar_->adjustSize();
            floatingToolbar_->move(point.x(), point.y() - floatingToolbar_->height() - 6);
            floatingToolbar_->show();
        });
        status_ = new QLabel;
        status_->setWordWrap(true);
        status_->hide();
        layout->addWidget(status_);
        auto actions = new QHBoxLayout;
        layout->addLayout(actions);
        auto discard = button("Discard draft", actions, [this] {
            if (!id_.isEmpty())
                session_.moveLetter(id_, "Trash");
            dirty_ = false;
            QDialog::done(QDialog::Rejected);
        });
        discard->setObjectName("discardButton");
        actions->addStretch();
        button("Save a draft", actions, [this] {
            dirty_ = true;
            if (save())
                accept();
        });
        auto send = button("Send", actions, [this] {
            dirty_ = true;
            if (save() && session_.sendLetter(id_))
                accept();
            else {
                status_->setText(session_.error());
                status_->show();
            }
        });
        send->setObjectName("sendButton");
        send->setDefault(true);
        id_ = reply ? QString() : letter["hash"].toString();
        dirty_ = reply;
        auto changed = [this] {
            dirty_ = true;
            autosave_.start(800);
        };
        autosave_.setSingleShot(true);
        connect(&autosave_, &QTimer::timeout, this, [this] { save(); });
        connect(body_, &QTextEdit::textChanged, this, [this, changed] {
            bodyEdited_ = true;
            changed();
        });
        connect(to_, &QLineEdit::textEdited, this, changed);
        connect(subject_, &QLineEdit::textEdited, this, changed);
        connect(sender_, &QComboBox::currentIndexChanged, this, changed);
        connect(modePublic_, &QPushButton::toggled, this, [this, changed](bool checked) {
            to_->setVisible(!checked);
            changed();
        });
        to_->setVisible(!modePublic_->isChecked());
        connect(&session_, &Session::aboutToCloseMailbox, this, [this] {
            if (save())
                accept();
        });
    }
    void reject() override {
        if (save())
            QDialog::reject();
    }
};
// The plain / text / markdown / hex switch. Owns its buttons rather than
// having them found by name, because the pop-out window is a child of the
// main window: two sets of identically named buttons would otherwise make
// every findChild lookup ambiguous.
class ViewSwitch : public QWidget {
  public:
    ViewSwitch(QColor iconColor, std::function<void(BodyView)> changed) {
        setProperty("viewSwitch", true); // styled by property: both instances share the rules
        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto group = new QButtonGroup(this);
        group->setExclusive(true);
        for (int i = 0; i < 4; ++i) {
            auto btn = buttons_[i] = new QToolButton;
            btn->setObjectName(QString("view_") + kViews[i].id);
            btn->setCheckable(true);
            btn->setFixedSize(28, 28); // matches the actions pill's height beside it
            btn->setIconSize(QSize(20, 20));
            btn->setToolTip(kViews[i].label);
            btn->setCursor(Qt::PointingHandCursor);
            group->addButton(btn);
            // clicked(), not toggled(): setMode() checks a button itself after
            // detection, and that must not bounce back as a second render.
            QObject::connect(btn, &QToolButton::clicked, this, [this, changed] { changed(mode()); });
            layout->addWidget(btn);
        }
        setIconColor(iconColor);
    }
    BodyView mode() const {
        for (int i = 0; i < 4; ++i)
            if (buttons_[i]->isChecked())
                return BodyView(i);
        return BodyView::Plain;
    }
    void setMode(BodyView mode) {
        buttons_[int(mode)]->setChecked(true);
    }
    void setIconColor(QColor color) {
        for (int i = 0; i < 4; ++i)
            buttons_[i]->setIcon(materialIcon(kViews[i].icon, color));
    }

  private:
    // Indexed by BodyView.
    static constexpr struct { const char *id, *icon, *label; } kViews[] = {
        {"plain", "viewPlain", "Plain text, fixed width"},
        {"text", "viewText", "Text"},
        {"markdown", "viewMarkdown", "Markdown"},
        {"hex", "viewHex", "Hex"},
    };
    QToolButton *buttons_[4];
};
// Shows a letter in the given mode. Plain and hex are the fixed-width modes,
// and the subject rides along: if a body needed a monospace grid to make
// sense, its subject does too.
// Hex for a subject line: a run of byte pairs, capped -- a cryptic subject
// can be tens of kilobytes, and this is a header, not the dump.
QString hexLine(const QByteArray &bytes, int max = 48) {
    QStringList pairs;
    for (int i = 0; i < qMin<int>(bytes.size(), max); ++i)
        pairs << QString("%1").arg(quint8(bytes[i]), 2, 16, QChar('0'));
    auto line = pairs.join(' ');
    if (bytes.size() > max)
        line += QString(" \u2026 (%1 bytes)").arg(bytes.size());
    return line;
}
void showLetterBody(QTextBrowser *body, QLabel *subject, const QString &subjectText,
                    const QString &text, BodyView mode) {
    renderBody(body, text, mode);
    const auto line = singleLine(subjectText);
    subject->setText(mode == BodyView::Hex ? hexLine(subjectText.toUtf8()) : line);
    const bool fixed = mode == BodyView::Plain || mode == BodyView::Hex;
    subject->setFont(fixed || line.contains("BM-") ? addressFont() : QApplication::font());
}
class MessageWindow : public QDialog {
  public:
    MessageWindow(Session &session, QVariantMap letter, bool dark, QWidget *parent)
        : QDialog(parent), session_(session), letter_(std::move(letter)), dark_(dark) {
        setObjectName("messageWindow");
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(singleLine(letter_["subject"].toString()).left(80));
        resize(640, 560);
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        auto letterFrame = new KindFrame;
        letterFrame->setObjectName("windowKindStripe");
        layout->addWidget(letterFrame, 1);
        auto frame = letterFrame->contentLayout();
        auto toolbar = new QToolBar;
        toolbar->setObjectName("windowActionsToolbar");
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setIconSize(QSize(22, 22));
        auto replyAction = toolbar->addAction(materialIcon("reply", iconColor(dark)), "Reply");
        connect(replyAction, &QAction::triggered, this, [this] {
            Composer dialog(session_, letter_, true, dark_, this);
            dialog.exec();
        });
        auto archiveAction = toolbar->addAction(materialIcon("archive", iconColor(dark)), "Archive");
        connect(archiveAction, &QAction::triggered, this,
                [this] { session_.moveLetter(letter_["hash"].toString(), "Archive"); });
        auto trashAction = toolbar->addAction(materialIcon("delete", iconColor(dark)), "Trash");
        connect(trashAction, &QAction::triggered, this,
                [this] { session_.moveLetter(letter_["hash"].toString(), "Trash"); });
        auto more = new QToolButton;
        more->setIcon(materialIcon("more", iconColor(dark)));
        more->setToolTip("More");
        more->setPopupMode(QToolButton::InstantPopup);
        auto menu = new QMenu(more);
        more->setMenu(menu);
        toolbar->addWidget(more);
        menu->addAction("Copy subject", this,
                        [this] { QApplication::clipboard()->setText(letter_["subject"].toString()); });
        const bool trashed = letter_["folder"] == "Trash";
        const bool outgoing = letter_["folder"] == "Outbox";
        menu->addAction("Restore", this, [this] { session_.restoreLetter(letter_["hash"].toString()); })
            ->setVisible(trashed);
        menu->addAction("Delete permanently", this,
                        [this] { session_.deleteLetter(letter_["hash"].toString()); })
            ->setVisible(trashed);
        menu->addAction("Retry", this, [this] { session_.retryLetter(letter_["hash"].toString()); })
            ->setVisible(outgoing);
        menu->addAction("Cancel delivery", this,
                        [this] { session_.cancelLetter(letter_["hash"].toString()); })
            ->setVisible(outgoing);
        auto toolRow = new QHBoxLayout;
        toolRow->setContentsMargins(0, 0, 0, 0);
        toolRow->addWidget(toolbar);
        toolRow->addStretch();
        frame->addLayout(toolRow);
        auto subject = subjectArea(frame, "windowSubjectLabel", "windowSubjectScroll");

        auto addresses =
            new QLabel(letter_["from"].toString() + "  →  " + letter_["to"].toString());
        addresses->setObjectName("windowAddresses");
        addresses->setFont(addressFont());
        addresses->setTextFormat(Qt::PlainText);
        addresses->setWordWrap(true);
        addresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
        frame->addWidget(addresses);
        auto body = new QTextBrowser;
        body->setObjectName("windowBody");
        body->setDocument(new SafeDocument(body));
        new AddressHighlighter(body->document());
        body->setOpenLinks(false);
        body->setFrameShape(QFrame::NoFrame);
        body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        connect(body, &QTextBrowser::anchorClicked, this, [this](QUrl url) {
            if (url.scheme() == "https" &&
                QMessageBox::question(this, "Open link",
                                      "Open this link in your browser?\n" +
                                          url.toDisplayString()) == QMessageBox::Yes)
                QDesktopServices::openUrl(url);
        });
        frame->addWidget(body, 1);
        const auto text = letter_["body"].toString();
        const auto subjectText = letter_["subject"].toString();
        auto views = new ViewSwitch(iconColor(dark), [body, subject, subjectText, text](BodyView mode) {
            showLetterBody(body, subject, subjectText, text, mode);
        });
        views->setObjectName("windowViewSwitch");
        toolRow->addWidget(views);
        views->setMode(detectBodyView(subjectText, text));
        showLetterBody(body, subject, subjectText, text, views->mode());
        letterFrame->setKind(classifyLetter(letter_["folder"].toString(),
                                            letter_["from"].toString(), letter_["to"].toString()));
    }

  private:
    Session &session_;
    QVariantMap letter_;
    bool dark_;
};
} // namespace
DesktopWindow::DesktopWindow(Session &session) : session_(session) {
    setObjectName("desktopWindow");
    listDensity_ = QSettings().value("listDensity", "comfortable").toString();
    if (listDensity_ != "compact" && listDensity_ != "cozy" && listDensity_ != "comfortable")
        listDensity_ = "comfortable";
    resize(1160, 780);
    setMinimumSize(900, 620);
    auto central = new QWidget;
    setCentralWidget(central);
    auto outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto header = new QWidget;
    header->setObjectName("header");
    auto top = new QHBoxLayout(header);
    top->setContentsMargins(24, 18, 24, 18);
    auto logo = new QLabel;
    logo->setPixmap(appLogo().pixmap(40, 40));
    logo->setFixedSize(40, 40);
    top->addWidget(logo);
    auto brand = new QLabel(
        "<b style='font-size:20px'>ynotbit</b><br><span "
        "style='font-size:10px'>PRIVATE CORRESPONDENCE</span><br><span "
        "style='font-size:9px;color:palette(mid)'>Your keys. Your mailbox.</span>");
    top->addWidget(brand);
    top->addStretch();
    auto adBanner = new QPushButton("🌙  NightTrader Exchange — your keys, your coins");
    adBanner->setObjectName("adBanner");
    adBanner->setCursor(Qt::PointingHandCursor);
    adBanner->setToolTip("https://retro.nighttrader.exchange");
    adBanner->setStyleSheet(
        "QPushButton{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #2b1055,stop:1 "
        "#7597de);color:white;border:0;border-radius:8px;padding:8px 18px;font-size:12px;"
        "font-weight:600;} QPushButton:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        "stop:0 #34136a,stop:1 #86a5e6);}");
    connect(adBanner, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl("https://retro.nighttrader.exchange")); });
    top->addWidget(adBanner);
    top->addStretch();
    button("Close mailbox", top, [this] { session_.closeMailbox(); })
        ->setObjectName("closeMailboxButton");
    button("Lock vault", top, [this] { session_.lock(); })->setObjectName("lockButton");
    outer->addWidget(header);
    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setContentsMargins(20, 8, 20, 8);
    outer->addWidget(error_);
    auto splitWidget = new QWidget;
    auto split = new QHBoxLayout(splitWidget);
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);
    outer->addWidget(splitWidget, 1);
    auto side = sidebarWidget_ = new QWidget;
    side->setObjectName("sidebar");
    side->setFixedWidth(66);
    auto nav = new QVBoxLayout(side);
    nav->setContentsMargins(12, 14, 12, 14);
    nav->setSpacing(6);
    nav->setAlignment(Qt::AlignHCenter);
    auto write = new QPushButton;
    write->setObjectName("writeButton");
    write->setFixedSize(38, 38);
    write->setIconSize(QSize(24, 24));
    write->setIcon(materialIcon("edit", iconColor(appearance_.dark())));
    write->setCursor(Qt::PointingHandCursor);
    connect(write, &QPushButton::clicked, this, [this] {
        if (folders_->currentRow() == 4 && !activeChannelAddress_.isEmpty())
            compose({{"to", activeChannelAddress_}, {"from", activeChannelAddress_}});
        else
            compose();
    });
    nav->addWidget(write);
    nav->addSpacing(4);
    auto divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setFixedWidth(32);
    divider->setStyleSheet("background:palette(mid);max-height:1px;border:0;");
    nav->addWidget(divider);
    nav->addSpacing(2);
    // folders_ stays the selection model driving all existing folder-change logic
    // below; the visible UI is the icon rail built from kFolderIcons instead.
    folders_ = new QListWidget(side);
    folders_->setObjectName("folders");
    folders_->addItems({"Inbox", "Drafts", "Outbox", "Sent", "Channels", "Broadcasts", "Archive",
                        "Trash", "Identities"});
    folders_->hide();
    auto folderGroup = new QButtonGroup(this);
    folderGroup->setExclusive(true);
    for (int i = 0; i < kFolderIcons.size(); ++i) {
        const auto &[label, iconName] = kFolderIcons[i];
        auto icon = new QToolButton;
        icon->setObjectName("folderIcon_" + label);
        icon->setCheckable(true);
        icon->setChecked(i == 0);
        icon->setFixedSize(38, 38);
        icon->setIconSize(QSize(24, 24));
        icon->setIcon(materialIcon(iconName, iconColor(appearance_.dark())));
        icon->setToolTip(label);
        icon->setCursor(Qt::PointingHandCursor);
        folderGroup->addButton(icon);
        connect(icon, &QToolButton::clicked, this, [this, i] { folders_->setCurrentRow(i); });
        nav->addWidget(icon);
    }
    nav->addStretch();
    split->addWidget(side);
    auto rail = channelRail_ = new QWidget;
    rail->setObjectName("channelRail");
    rail->setFixedWidth(190);
    auto railOuter = new QVBoxLayout(rail);
    railOuter->setContentsMargins(10, 5, 2, 12);
    railOuter->setSpacing(4);
    auto railHead = new QLabel("CHANNELS");
    railHead->setStyleSheet("font-size:11px;font-weight:700;color:palette(mid);");
    railOuter->addWidget(railHead);
    auto railScroll = new QScrollArea;
    railScroll->setObjectName("channelRailScroll");
    railScroll->setWidgetResizable(true);
    railScroll->setFrameShape(QFrame::NoFrame);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto railList = new QWidget;
    channelChipLayout_ = new QVBoxLayout(railList);
    channelChipLayout_->setContentsMargins(0, 4, 0, 0);
    channelChipLayout_->setSpacing(2);
    railScroll->setWidget(railList);
    railOuter->addWidget(railScroll, 1);
    button("+ Join or create…", railOuter, [this] {
        session_.joinChannel();
        refreshChannels();
    })->setObjectName("joinOrCreateChannelButton");
    split->addWidget(rail);
    auto middle = listColumn_ = new QWidget;
    middle->setObjectName("listColumn");
    middle->setFixedWidth(300);
    auto mid = new QVBoxLayout(middle);
    mid->setContentsMargins(3, 5, 3, 0);
    mid->setSpacing(2);
    heading_ = new QLabel("INBOX");
    heading_->setObjectName("listHeading");
    heading_->setStyleSheet("font-size:11px;font-weight:700;color:palette(mid);");
    mid->addWidget(heading_);
    auto search = search_ = new QLineEdit;
    search->setPlaceholderText("Search this folder");
    mid->addWidget(search);
    auto debounce = new QTimer(this);
    debounce->setSingleShot(true);
    connect(search, &QLineEdit::textChanged, this, [debounce] { debounce->start(250); });
    connect(debounce, &QTimer::timeout, this, [this, search] {
        session_.messageModel()->setSearch(search->text());
        updateListCount();
    });
    auto densityRow = new QHBoxLayout;
    densityRow->setContentsMargins(0, 0, 0, 0);
    auto filterSwitch = new QWidget;
    filterSwitch->setObjectName("filterSwitch");
    auto filterSwitchLayout = new QHBoxLayout(filterSwitch);
    filterSwitchLayout->setContentsMargins(0, 0, 0, 0);
    filterSwitchLayout->setSpacing(0);
    const struct { const char *id, *icon, *label; } filters[] = {
        {"unread", "filterUnread", "Unread only"},
        {"anonymous", "filterAnonymous", "Anonymous only"},
    };
    for (const auto &f : filters) {
        auto btn = new QToolButton;
        btn->setObjectName(QString("filter_") + f.id);
        btn->setCheckable(true);
        btn->setFixedSize(26, 26);
        btn->setIconSize(QSize(16, 16));
        btn->setIcon(materialIcon(f.icon, iconColor(appearance_.dark())));
        btn->setToolTip(f.label);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QToolButton::toggled, this, [this, id = QString(f.id)](bool on) {
            if (id == "unread")
                session_.messageModel()->setUnreadOnly(on);
            else
                session_.messageModel()->setAnonymousOnly(on);
            updateListCount();
        });
        filterSwitchLayout->addWidget(btn);
    }
    densityRow->addWidget(filterSwitch);
    densityRow->addStretch();
    listCountLabel_ = new QLabel;
    listCountLabel_->setObjectName("listCountLabel");
    listCountLabel_->setStyleSheet("font-size:11px;color:palette(mid);");
    densityRow->addWidget(listCountLabel_);
    densityRow->addStretch();
    auto densitySwitch = new QWidget;
    densitySwitch->setObjectName("densitySwitch");
    auto densitySwitchLayout = new QHBoxLayout(densitySwitch);
    densitySwitchLayout->setContentsMargins(0, 0, 0, 0);
    densitySwitchLayout->setSpacing(0);
    auto densityGroup = new QButtonGroup(this);
    densityGroup->setExclusive(true);
    const struct { const char *id, *icon, *label; } densities[] = {
        {"comfortable", "densityComfortable", "Comfortable"},
        {"cozy", "densityCozy", "Cozy"},
        {"compact", "densityCompact", "Compact"},
    };
    for (const auto &d : densities) {
        auto btn = new QToolButton;
        btn->setObjectName(QString("density_") + d.id);
        btn->setCheckable(true);
        btn->setChecked(listDensity_ == d.id);
        btn->setFixedSize(26, 26);
        btn->setIconSize(QSize(16, 16));
        btn->setIcon(materialIcon(d.icon, iconColor(appearance_.dark())));
        btn->setToolTip(d.label);
        btn->setCursor(Qt::PointingHandCursor);
        densityGroup->addButton(btn);
        connect(btn, &QToolButton::clicked, this, [this, id = QString(d.id)] {
            setListDensity(id);
        });
        densitySwitchLayout->addWidget(btn);
    }
    densityRow->addWidget(densitySwitch);
    mid->addLayout(densityRow);
    letters_ = new QListView;
    letters_->setObjectName("letters");
    letters_->setModel(session_.messageModel());
    letterDelegate_ = new LetterDelegate(letters_, listDensity_);
    letters_->setItemDelegate(letterDelegate_);
    letters_->setUniformItemSizes(true);
    letters_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    letters_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mid->addWidget(letters_, 1);
    split->addWidget(middle);
    reader_ = new QWidget;
    auto read = new QVBoxLayout(reader_);
    read->setContentsMargins(0, 0, 0, 0);
    read->setSpacing(14);
    auto toolbar = new QToolBar;
    toolbar->setObjectName("actionsToolbar");
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(22, 22));
    auto editAction =
        toolbar->addAction(materialIcon("edit", iconColor(appearance_.dark())), "Edit / Send");
    editAction->setObjectName("editAction");
    connect(editAction, &QAction::triggered, this, [this] { compose(selected_); });
    auto replyAction = toolbar->addAction(materialIcon("reply", iconColor(appearance_.dark())), "Reply");
    replyAction->setObjectName("replyAction");
    connect(replyAction, &QAction::triggered, this, [this] { compose(selected_, true); });
    auto archiveAction =
        toolbar->addAction(materialIcon("archive", iconColor(appearance_.dark())), "Archive");
    archiveAction->setObjectName("archiveAction");
    connect(archiveAction, &QAction::triggered, this,
            [this] { session_.moveLetter(selected_["hash"].toString(), "Archive"); });
    auto trashAction = toolbar->addAction(materialIcon("delete", iconColor(appearance_.dark())), "Trash");
    trashAction->setObjectName("trashAction");
    connect(trashAction, &QAction::triggered, this,
            [this] { session_.moveLetter(selected_["hash"].toString(), "Trash"); });
    auto more = new QToolButton;
    more->setObjectName("moreActionsButton");
    more->setIcon(materialIcon("more", iconColor(appearance_.dark())));
    more->setToolTip("More");
    more->setPopupMode(QToolButton::InstantPopup);
    auto menu = new QMenu(more);
    more->setMenu(menu);
    toolbar->addWidget(more);
    menu->addAction("Copy subject", this,
                    [this] { QApplication::clipboard()->setText(selected_["subject"].toString()); });
    menu->addAction("Open in new window", this,
                    [this] {
                        (new MessageWindow(session_, selected_, appearance_.dark(), this))->show();
                    });
    menu->addAction("Restore", this,
                    [this] { session_.restoreLetter(selected_["hash"].toString()); })
        ->setObjectName("restoreAction");
    menu->addAction("Delete permanently", this,
                    [this] { session_.deleteLetter(selected_["hash"].toString()); })
        ->setObjectName("deletePermanentlyAction");
    menu->addAction("Retry", this, [this] { session_.retryLetter(selected_["hash"].toString()); })
        ->setObjectName("retryAction");
    menu->addAction("Cancel delivery", this,
                    [this] { session_.cancelLetter(selected_["hash"].toString()); })
        ->setObjectName("cancelDeliveryAction");
    menu->addAction("Delivery history", this, [this] {
        QString text;
        for (auto v : session_.deliveryHistory(selected_["hash"].toString())) {
            auto m = v.toMap();
            text += m["time"].toString() + " · " + m["state"].toString() + "\n" +
                    m["detail"].toString() + "\n\n";
        }
        QMessageBox::information(this, "Delivery history", text);
    });
    menu->addAction("Full subject", this, [this] {
        QMessageBox dialog(QMessageBox::Information, "Full subject",
                           selected_["subject"].toString(), QMessageBox::Ok, this);
        dialog.setTextFormat(Qt::PlainText);
        if (selected_["subject"].toString().contains("BM-"))
            dialog.setFont(addressFont());
        dialog.exec();
    });
    auto letterFrame = new KindFrame;
    letterFrame->setObjectName("letterKindStripe");
    letterStripe_ = letterFrame;
    auto frame = letterFrame->contentLayout();
    // The letter's own actions on the left, how it is rendered on the right.
    auto toolRow = new QWidget;
    auto toolRowLayout = new QHBoxLayout(toolRow);
    toolRowLayout->setContentsMargins(0, 0, 0, 0);
    toolRowLayout->addWidget(toolbar);
    toolRowLayout->addStretch();
    auto viewSwitch = new ViewSwitch(iconColor(appearance_.dark()), [this](BodyView) { renderSelectedBody(); });
    viewSwitch->setObjectName("viewSwitch");
    viewSwitch_ = viewSwitch;
    toolRowLayout->addWidget(viewSwitch);
    actions_ = toolRow;
    frame->addWidget(actions_);
    read->addWidget(letterFrame, 1);
    subject_ = subjectArea(frame, "subjectLabel", "subjectScroll");
    subject_->setText("No letter selected");
    details_ = new QWidget;
    auto metadata = new QGridLayout(details_);
    metadata->setContentsMargins(0, 0, 0, 0);
    metadata->setHorizontalSpacing(14);
    metadata->setVerticalSpacing(6);
    metadata->setColumnStretch(1, 1);
    fromAddress_ = new QLabel;
    toAddress_ = new QLabel;
    fromAddress_->setObjectName("messageAddresses");
    toAddress_->setObjectName("toAddress");
    int addressRow = 0;
    for (auto field : {fromAddress_, toAddress_}) {
        field->setFont(addressFont());
        field->setTextFormat(Qt::PlainText);
        field->setWordWrap(true);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto label = new QLabel(addressRow == 0 ? "From" : "To");
        metadata->addWidget(label, addressRow, 0, Qt::AlignTop);
        metadata->addWidget(field, addressRow++, 1);
    }
    deliveryStatus_ = new QLabel;
    deliveryStatus_->setObjectName("deliveryStatus");
    deliveryStatus_->setTextFormat(Qt::PlainText);
    metadata->addWidget(deliveryStatus_, 2, 1, Qt::AlignLeft);
    deliveryError_ = new QLabel;
    deliveryError_->setTextFormat(Qt::PlainText);
    deliveryError_->setWordWrap(true);
    metadata->addWidget(deliveryError_, 3, 1);
    timeline_ = new QLabel;
    timeline_->setObjectName("messageTimeline");
    timeline_->setTextFormat(Qt::RichText);
    timeline_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timeline_->setToolTip("Times are local. Received in mailbox is when the object was decrypted "
                          "and saved, which may be after network arrival while locked. Sent to "
                          "peers is a relay offer, not a read receipt.");
    metadata->addWidget(timeline_, 4, 0, 1, 2);
    details_->hide();
    frame->addWidget(details_);
    body_ = new QTextBrowser;
    body_->setObjectName("readerBody");
    body_->setDocument(new SafeDocument(body_));
    new AddressHighlighter(body_->document());
    body_->setOpenLinks(false);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    frame->addWidget(body_, 1);
    connect(body_, &QTextBrowser::anchorClicked, this, [this](QUrl url) {
        if (url.scheme() == "https" &&
            QMessageBox::question(this, "Open link",
                                  "Open this link in your browser?\n" + url.toDisplayString()) ==
                QMessageBox::Yes)
            QDesktopServices::openUrl(url);
    });
    welcomeStack_ = new QStackedWidget;
    welcomeStack_->setObjectName("welcomeStack");
    // State 1: vault locked (or none chosen yet). A vault target is shown with an
    // inline password field instead of a modal dialog; recents let you switch targets.
    lockedPage_ = new QWidget;
    auto lockedOuter = new QVBoxLayout(lockedPage_);
    lockedOuter->addStretch();
    auto lockedCard = new QWidget;
    lockedCard->setFixedWidth(380);
    auto lockedCardLayout = new QVBoxLayout(lockedCard);
    lockedCardLayout->setSpacing(14);
    auto lockedTitle = new QLabel("<b style='font-size:16px'>Vault locked</b>");
    lockedCardLayout->addWidget(lockedTitle);
    auto lockedSubtitle = new QLabel("Choose a vault file and enter its passphrase to unlock.");
    lockedSubtitle->setStyleSheet("color:palette(mid);");
    lockedSubtitle->setWordWrap(true);
    lockedCardLayout->addWidget(lockedSubtitle);
    auto authGroup = new QWidget;
    authGroup->setObjectName("vaultAuthGroup");
    auto authLayout = new QVBoxLayout(authGroup);
    authLayout->setContentsMargins(0, 0, 0, 0);
    authLayout->setSpacing(8);
    auto vaultBox = new QWidget;
    vaultBox->setObjectName("vaultBox");
    vaultBox->setStyleSheet(
        "QWidget#vaultBox{border:1px solid palette(mid);border-radius:8px;padding:10px;}");
    auto vaultBoxLayout = new QVBoxLayout(vaultBox);
    lockedVaultName_ = new QLabel;
    lockedVaultName_->setObjectName("lockedVaultName");
    lockedVaultName_->setStyleSheet("font-weight:700;");
    lockedVaultPath_ = new QLabel;
    lockedVaultPath_->setObjectName("lockedVaultPath");
    lockedVaultPath_->setFont(addressFont());
    lockedVaultPath_->setWordWrap(true);
    lockedVaultPath_->setStyleSheet("color:palette(mid);font-size:11px;");
    vaultBoxLayout->addWidget(lockedVaultName_);
    vaultBoxLayout->addWidget(lockedVaultPath_);
    authLayout->addWidget(vaultBox);
    vaultPasswordField_ = new QLineEdit;
    vaultPasswordField_->setObjectName("vaultPasswordField");
    vaultPasswordField_->setEchoMode(QLineEdit::Password);
    vaultPasswordField_->setPlaceholderText("Passphrase");
    authLayout->addWidget(vaultPasswordField_);
    vaultRepeatField_ = new QLineEdit;
    vaultRepeatField_->setObjectName("vaultRepeatField");
    vaultRepeatField_->setEchoMode(QLineEdit::Password);
    vaultRepeatField_->setPlaceholderText("Repeat passphrase");
    authLayout->addWidget(vaultRepeatField_);
    vaultUnlockButton_ = new QPushButton("Unlock vault");
    vaultUnlockButton_->setObjectName("vaultUnlockButton");
    authLayout->addWidget(vaultUnlockButton_);
    lockedCardLayout->addWidget(authGroup);
    auto lockedRecentsLabel = new QLabel("RECENT VAULTS");
    lockedRecentsLabel->setStyleSheet("color:palette(mid);font-size:11px;font-weight:700;");
    lockedCardLayout->addWidget(lockedRecentsLabel);
    auto lockedRecentsContainer = new QWidget;
    lockedRecentsLayout_ = new QVBoxLayout(lockedRecentsContainer);
    lockedRecentsLayout_->setContentsMargins(0, 0, 0, 0);
    lockedCardLayout->addWidget(lockedRecentsContainer);
    button("Open vault file from disk…", lockedCardLayout,
           [this] { session_.beginVaultOpen(); })
        ->setObjectName("openVaultButton");
    button("Create a new vault…", lockedCardLayout, [this] { session_.beginVaultCreate(); })
        ->setObjectName("createVaultButton");
    lockedOuter->addWidget(lockedCard, 0, Qt::AlignHCenter);
    lockedOuter->addStretch();
    welcomeStack_->addWidget(lockedPage_);
    connect(vaultPasswordField_, &QLineEdit::returnPressed, vaultUnlockButton_,
            &QPushButton::click);
    connect(vaultUnlockButton_, &QPushButton::clicked, this, [this] {
        session_.choosePendingVault(targetVaultPath_);
        session_.submitVaultPassword(vaultPasswordField_->text(), vaultRepeatField_->text(),
                                     vaultCreateMode_);
        vaultPasswordField_->clear();
        vaultRepeatField_->clear();
    });
    // State 2: vault unlocked, no mailbox open yet.
    noMailboxPage_ = new QWidget;
    auto noMailOuter = new QVBoxLayout(noMailboxPage_);
    noMailOuter->addStretch();
    auto noMailCard = new QWidget;
    noMailCard->setFixedWidth(380);
    auto noMailLayout = new QVBoxLayout(noMailCard);
    noMailLayout->setSpacing(14);
    auto noMailTitle = new QLabel("<b style='font-size:16px'>No mailbox open</b>");
    noMailLayout->addWidget(noMailTitle);
    auto noMailSubtitle =
        new QLabel("Your vault is unlocked. Choose a mailbox to open, or open one from disk.");
    noMailSubtitle->setStyleSheet("color:palette(mid);");
    noMailSubtitle->setWordWrap(true);
    noMailLayout->addWidget(noMailSubtitle);
    auto noMailRecentsLabel = new QLabel("RECENT MAILBOXES");
    noMailRecentsLabel->setStyleSheet("color:palette(mid);font-size:11px;font-weight:700;");
    noMailLayout->addWidget(noMailRecentsLabel);
    auto noMailRecentsContainer = new QWidget;
    noMailboxRecentsLayout_ = new QVBoxLayout(noMailRecentsContainer);
    noMailboxRecentsLayout_->setContentsMargins(0, 0, 0, 0);
    noMailLayout->addWidget(noMailRecentsContainer);
    button("Open mailbox file from disk…", noMailLayout, [this] { session_.openMailbox(); })
        ->setObjectName("openMailboxButton");
    button("Create a new mailbox…", noMailLayout, [this] { session_.createMailbox(); })
        ->setObjectName("createMailboxButton");
    noMailOuter->addWidget(noMailCard, 0, Qt::AlignHCenter);
    noMailOuter->addStretch();
    welcomeStack_->addWidget(noMailboxPage_);
    read->addWidget(welcomeStack_, 1);
    identities_ = new QWidget;
    identities_->setObjectName("identitiesPane");
    auto identitiesOuter = new QVBoxLayout(identities_);
    identitiesOuter->setContentsMargins(4, 4, 4, 0);
    identitiesOuter->setSpacing(8);
    auto identitiesHeadRow = new QHBoxLayout;
    auto identitiesTitleCol = new QVBoxLayout;
    auto identitiesHeading = new QLabel("Identities & chans");
    identitiesHeading->setObjectName("identitiesHeading");
    identitiesHeading->setStyleSheet("font-size:20px;font-weight:600;");
    identitiesTitleCol->addWidget(identitiesHeading);
    auto identitiesSub = new QLabel("Addresses you can send mail from");
    identitiesSub->setStyleSheet("color:palette(mid);font-size:12px;");
    identitiesTitleCol->addWidget(identitiesSub);
    identitiesHeadRow->addLayout(identitiesTitleCol);
    identitiesHeadRow->addStretch();
    button("Join a channel…", identitiesHeadRow, [this] {
        session_.joinChannel();
        refreshIdentities();
    })->setObjectName("joinChannelButton");
    button("New identity", identitiesHeadRow, [this] {
        session_.addIdentity();
        refreshIdentities();
    })->setObjectName("newIdentityButton");
    identitiesOuter->addLayout(identitiesHeadRow);
    auto identitiesScroll = new QScrollArea;
    identitiesScroll->setObjectName("identitiesScroll");
    identitiesScroll->setWidgetResizable(true);
    identitiesScroll->setFrameShape(QFrame::NoFrame);
    identitiesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto identitiesListContainer = new QWidget;
    identityLayout_ = new QVBoxLayout(identitiesListContainer);
    identityLayout_->setContentsMargins(0, 4, 4, 16);
    identityLayout_->setSpacing(10);
    identitiesScroll->setWidget(identitiesListContainer);
    identitiesOuter->addWidget(identitiesScroll, 1);
    read->addWidget(identities_);
    split->addWidget(reader_, 1);
    status_ = new QLabel;
    status_->setObjectName("statusBar");
    status_->setContentsMargins(24, 12, 24, 12);
    outer->addWidget(status_);
    auto file = menuBar()->addMenu("File");
    file->addAction("Create vault…", &session_, &Session::beginVaultCreate);
    file->addAction("Open vault…", &session_, &Session::beginVaultOpen);
    file->addAction("Create mailbox…", &session_, &Session::createMailbox);
    file->addAction("Open mailbox…", &session_, &Session::openMailbox);
    file->addAction("Close mailbox", &session_, &Session::closeMailbox);
    file->addAction("Back up mailbox and vault…", &session_, &Session::backup);
    file->addAction("Lock vault", QKeySequence("Ctrl+L"), &session_, &Session::lock);
    auto network = menuBar()->addMenu("Network");
    auto enabled = network->addAction("Network enabled");
    enabled->setCheckable(true);
    enabled->setChecked(session_.networkEnabled());
    connect(enabled, &QAction::toggled, &session_, &Session::setNetworkEnabled);
    network->addAction("Peer / proxy settings…", &session_, &Session::configureNode);
    network->addAction("Restart node", &session_, &Session::restartNode);
    network->addAction("Retention settings…", &session_, &Session::configureRetention);
    auto identity = menuBar()->addMenu("Identity");
    identity->addAction("Create identity…", &session_, &Session::addIdentity);
    identity->addAction("Join or create chan…", &session_, &Session::joinChannel);
    identity->addAction("Import keys.dat…", &session_, &Session::importIdentities);
    identity->addAction("Change vault password…", &session_, &Session::changePassword);
    identity->addAction("Subscribe to broadcasts…", &session_, &Session::subscribe);
    identity->addAction("Manage subscriptions…", this, [this] {
        QStringList labels, addresses;
        for (auto v : session_.subscriptions()) {
            auto m = v.toMap();
            labels << m["label"].toString() + " · " + m["address"].toString();
            addresses << m["address"].toString();
        }
        bool ok;
        QInputDialog dialog(this);
        dialog.setWindowTitle("Unsubscribe");
        dialog.setLabelText("Subscription");
        dialog.setComboBoxItems(labels);
        dialog.setComboBoxEditable(false);
        dialog.setFont(addressFont());
        ok = dialog.exec() == QDialog::Accepted;
        auto value = dialog.textValue();
        if (ok && labels.contains(value))
            session_.unsubscribe(addresses[labels.indexOf(value)]);
    });
    identity->addAction("Inspect retained objects again", &session_, &Session::rescan);
    auto appearance = menuBar()->addMenu("Appearance");
    auto group = new QActionGroup(this);
    for (auto mode : {QString("system"), QString("light"), QString("dark")}) {
        auto a = appearance->addAction(mode);
        a->setCheckable(true);
        a->setChecked(appearance_.mode() == mode);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode] { appearance_.setMode(mode); });
    }
    connect(&appearance_, &Appearance::changed, this, &DesktopWindow::updateTheme);
    connect(&session_, &Session::changed, this, &DesktopWindow::updateState);
    connect(&session_, &Session::messagesChanged, this, &DesktopWindow::refreshChannels);
    connect(
        &session_, &Session::messagesChanged, this,
        [this] {
            if (!selected_.value("hash").toString().isEmpty())
                updateTimeline();
        },
        Qt::QueuedConnection);
    // Let the controller leave its guarded operation before the inline password
    // field submits a password through that same controller.
    connect(&session_, &Session::vaultPasswordRequired, this, &DesktopWindow::showVaultPasswordFor,
            Qt::QueuedConnection);
    connect(&session_, &Session::vaultPasswordAccepted, this, [this] {
        vaultPasswordField_->clear();
        vaultRepeatField_->clear();
    });
    connect(&session_, &Session::aboutToCloseMailbox, this, [this] {
        selected_.clear();
        body_->clear();
        subject_->setText("No letter selected");
        clearDetails();
        actions_->hide();
    });
    connect(letters_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](QModelIndex i) {
                if (i.isValid())
                    selectMessage(i.data(Qt::UserRole + 1).toString());
                else {
                    selected_.clear();
                    body_->clear();
                    subject_->setText("No letter selected");
                    clearDetails();
                    actions_->hide();
                }
            });
    connect(letters_, &QListView::doubleClicked, this, [this](QModelIndex i) {
        if (!i.isValid())
            return;
        auto message = session_.message(i.data(Qt::UserRole + 1).toString());
        if (message["folder"] == "Drafts")
            compose(message);
        else
            (new MessageWindow(session_, message, appearance_.dark(), this))->show();
    });
    connect(session_.messageModel(), &QAbstractItemModel::modelReset, this, [this] {
        const auto hash = selected_.value("hash").toString();
        const int row = session_.messageModel()->rowForHash(hash);
        if (row >= 0) {
            // New mail arriving reloads the whole model; re-anchor on the same
            // message instead of dropping the reader pane out from under
            // whoever is mid-read.
            const auto idx = session_.messageModel()->index(row);
            letters_->setCurrentIndex(idx);
            letters_->scrollTo(idx);
            return;
        }
        selected_.clear();
        body_->clear();
        subject_->setText("No letter selected");
        clearDetails();
        actions_->hide();
    });
    connect(folders_, &QListWidget::currentTextChanged, this, [this](QString folder) {
        if (auto icon = findChild<QToolButton *>("folderIcon_" + folder))
            icon->setChecked(true);
        heading_->setText(folder.toUpper());
        if (folder == "Channels")
            refreshChannels();
        session_.messageModel()->setFolder(folder);
        selected_.clear();
        body_->clear();
        subject_->setText(folder == "Identities" ? "Identities & chans" : "No letter selected");
        clearDetails();
        actions_->hide();
        if (folder == "Identities")
            refreshIdentities();
        updateState();
    });
    folders_->setCurrentRow(0);
    updateTheme();
    updateState();
}
void DesktopWindow::updateTheme() {
    bool dark = appearance_.dark();
    setStyleSheet(
        QString(
            "QWidget{font-size:13px;} QMainWindow,QDialog{background:%1;} "
            "QWidget#sidebar{background:%2;} QWidget#header{background:%3;} "
            "QWidget#statusBar{background:%3;font-size:11px;} "
            "QPushButton,QLineEdit,QComboBox{padding:9px;border:1px solid %4;border-radius:6px;} "
            "QPushButton:hover{background:%2;} QListView,QTextEdit{border:0;} "
            "QListWidget::item{padding:10px;} QListWidget::item:selected{background:%5;color:%6;} "
            "QToolBar{border:0;spacing:3px;} "
            "QPushButton#writeButton{background:transparent;border:1px solid %4;"
            "border-radius:8px;padding:0;} "
            "QPushButton#writeButton:hover{background:%3;} "
            "QWidget#sidebar QToolButton{border:1px solid %4;border-radius:8px;"
            "background:transparent;padding:0;} "
            "QWidget#sidebar QToolButton:hover{background:%3;} "
            "QWidget#sidebar QToolButton:checked{background:%5;} "
            "QWidget#densitySwitch{border:1px solid %4;border-radius:7px;background:transparent;} "
            "QWidget#densitySwitch QToolButton{border:0;border-radius:0;"
            "background:transparent;padding:0;} "
            "QWidget#densitySwitch QToolButton:hover{background:%3;} "
            "QWidget#densitySwitch QToolButton:checked{background:%5;} "
            "QWidget#densitySwitch QToolButton#density_comfortable{"
            "border-top-left-radius:6px;border-bottom-left-radius:6px;"
            "border-right:1px solid %4;} "
            "QWidget#densitySwitch QToolButton#density_cozy{border-right:1px solid %4;} "
            "QWidget#densitySwitch QToolButton#density_compact{"
            "border-top-right-radius:6px;border-bottom-right-radius:6px;} "
            "QWidget#filterSwitch{border:1px solid %4;border-radius:7px;background:transparent;} "
            "QWidget#filterSwitch QToolButton{border:0;border-radius:0;"
            "background:transparent;padding:0;} "
            "QWidget#filterSwitch QToolButton:hover{background:%3;} "
            "QWidget#filterSwitch QToolButton:checked{background:%5;} "
            "QWidget#filterSwitch QToolButton#filter_unread{"
            "border-top-left-radius:6px;border-bottom-left-radius:6px;"
            "border-right:1px solid %4;} "
            "QWidget#filterSwitch QToolButton#filter_anonymous{"
            "border-top-right-radius:6px;border-bottom-right-radius:6px;} "
            "QToolBar#actionsToolbar,QToolBar#windowActionsToolbar{"
            "border:1px solid %4;border-radius:8px;background:transparent;"
            "spacing:0;padding:1px;} "
            "QToolBar#actionsToolbar QToolButton,QToolBar#windowActionsToolbar QToolButton{"
            "border:0;border-radius:0;background:transparent;padding:2px;} "
            "QToolBar#actionsToolbar QToolButton:hover,"
            "QToolBar#windowActionsToolbar QToolButton:hover{background:%3;} "
            "QWidget[viewSwitch=\"true\"]{border:1px solid %4;border-radius:7px;background:transparent;} "
            "QWidget[viewSwitch=\"true\"] QToolButton{border:0;border-radius:0;"
            "background:transparent;padding:0;} "
            "QWidget[viewSwitch=\"true\"] QToolButton:hover{background:%3;} "
            "QWidget[viewSwitch=\"true\"] QToolButton:checked{background:%5;} "
            "QWidget[viewSwitch=\"true\"] QToolButton#view_plain{"
            "border-top-left-radius:6px;border-bottom-left-radius:6px;} "
            "QWidget[viewSwitch=\"true\"] QToolButton#view_plain,QWidget[viewSwitch=\"true\"] QToolButton#view_text,"
            "QWidget[viewSwitch=\"true\"] QToolButton#view_markdown{border-right:1px solid %4;} "
            "QWidget[viewSwitch=\"true\"] QToolButton#view_hex{"
            "border-top-right-radius:6px;border-bottom-right-radius:6px;}")
            .arg(dark ? "#141b23" : "#f5f7fa", dark ? "#17212b" : "#f1f5f7",
                 dark ? "#1b2530" : "#ffffff", dark ? "#354553" : "#dbe3e8",
                 dark ? "#234b46" : "#dcebe8", dark ? "#73d8c7" : "#126d65"));
    letters_->viewport()->update();
    updateDeliveryStatus();
    const auto color = iconColor(dark);
    findChild<QAction *>("editAction")->setIcon(materialIcon("edit", color));
    findChild<QAction *>("replyAction")->setIcon(materialIcon("reply", color));
    findChild<QAction *>("archiveAction")->setIcon(materialIcon("archive", color));
    findChild<QAction *>("trashAction")->setIcon(materialIcon("delete", color));
    findChild<QToolButton *>("moreActionsButton")->setIcon(materialIcon("more", color));
    findChild<QPushButton *>("writeButton")->setIcon(materialIcon("edit", color));
    static_cast<ViewSwitch *>(viewSwitch_)->setIconColor(color);
    for (const auto &[id, icon] : {std::pair{"density_comfortable", "densityComfortable"},
                                   {"density_cozy", "densityCozy"},
                                   {"density_compact", "densityCompact"},
                                   {"filter_unread", "filterUnread"},
                                   {"filter_anonymous", "filterAnonymous"}})
        findChild<QToolButton *>(id)->setIcon(materialIcon(icon, color));
    for (const auto &[label, iconName] : kFolderIcons)
        findChild<QToolButton *>("folderIcon_" + label)->setIcon(materialIcon(iconName, color));
}
void DesktopWindow::clearDetails() {
    fromAddress_->clear();
    toAddress_->clear();
    deliveryStatus_->clear();
    deliveryError_->clear();
    timeline_->clear();
    details_->hide();
    static_cast<KindFrame *>(letterStripe_)->setKind(LetterKind::Personal);
}
void DesktopWindow::updateDeliveryStatus() {
    const auto state = selected_.value("state").toString();
    const bool success = state == "acknowledged";
    auto label = state;
    label.replace('_', ' ');
    if (!label.isEmpty())
        label[0] = label[0].toUpper();
    deliveryStatus_->setText(label);
    deliveryStatus_->setVisible(!label.isEmpty());
    deliveryStatus_->setToolTip(
        success ? "Recipient acknowledged delivery. This is not a read receipt." : QString());
    const bool dark = appearance_.dark();
    deliveryStatus_->setStyleSheet(
        QString(
            "QLabel {color:%1;background:%2;border-radius:6px;padding:4px 9px;font-weight:600;}")
            .arg(success ? (dark ? "#8ce0b2" : "#17643b") : (dark ? "#c1cdd7" : "#435867"),
                 success ? (dark ? "#183d2b" : "#e0f3e7") : (dark ? "#263440" : "#e8edf1")));
}
void DesktopWindow::updateTimeline() {
    const auto events = session_.deliveryHistory(selected_.value("hash").toString());
    QString html = "<table cellspacing='0' cellpadding='2'>";
    auto row = [&](const QString &label, const QString &time) {
        html += "<tr><td style='padding-right:16px'>" + label.toHtmlEscaped() + "</td><td>" +
                time.toHtmlEscaped() + "</td></tr>";
    };
    if (events.isEmpty()) {
        const auto folder = selected_.value("folder").toString();
        const auto label = folder == "Drafts" ? "Draft saved"
                                              : (folder == "Inbox" || folder == "Channels" ||
                                                         folder == "Broadcasts"
                                                     ? "Received in mailbox"
                                                     : "Saved in mailbox");
        row(label, QDateTime::fromSecsSinceEpoch(selected_.value("storedAt").toLongLong())
                       .toString("dd MMM yyyy · HH:mm:ss"));
    } else {
        const QMap<QString, QString> labels{{"queued", "Queued"},
                                            {"awaiting_pubkey", "Requested recipient key"},
                                            {"key_available", "Recipient key available"},
                                            {"calculating_ack", "Receipt preparation started"},
                                            {"calculating_message", "Message preparation started"},
                                            {"prepared", "Prepared"},
                                            {"publishing", "Submitted to relay"},
                                            {"published", "Sent to peers"},
                                            {"awaiting_ack", "Sent to peers"},
                                            {"acknowledged", "Acknowledged"},
                                            {"failed", "Failed"},
                                            {"expired", "Expired"},
                                            {"cancelled", "Cancelled"}};
        // Show the latest occurrence of each recorded stage; the full history
        // remains available in More, including retries and detailed explanations.
        QMap<QString, int> latest;
        for (int i = 0; i < events.size(); ++i)
            latest[events[i].toMap().value("state").toString()] = i;
        for (int i = 0; i < events.size(); ++i) {
            const auto event = events[i].toMap();
            const auto state = event.value("state").toString();
            if (labels.contains(state) && latest.value(state) == i)
                row(labels.value(state), event.value("time").toString());
            if (state != "key_available" && state != "prepared")
                selected_["state"] = state;
        }
        const auto current = selected_.value("state").toString();
        bool currentShown = false;
        for (const auto &event : events)
            if (event.toMap().value("state").toString() == current)
                currentShown = true;
        if (!current.isEmpty() && !currentShown) {
            auto label = current;
            label.replace('_', ' ');
            if (!label.isEmpty())
                label[0] = label[0].toUpper();
            row(label, QDateTime::currentDateTime().toString("dd MMM yyyy · HH:mm:ss"));
        }
    }
    timeline_->setText(html + "</table>");
    updateDeliveryStatus();
}
void DesktopWindow::updateState() {
    setWindowTitle(session_.document() + " — ynotbit");
    error_->setText(session_.error());
    error_->setVisible(!session_.error().isEmpty());
    findChild<QPushButton *>("lockButton")->setVisible(session_.unlocked());
    findChild<QPushButton *>("closeMailboxButton")->setVisible(session_.mailboxOpen());
    const bool channelPage = folders_->currentRow() == 4;
    const bool mailboxState = session_.mailboxOpen();
    auto write = findChild<QPushButton *>("writeButton");
    write->setToolTip(channelPage ? "Write to channel" : "Write a letter");
    write->setEnabled(mailboxState && (!channelPage || !activeChannelAddress_.isEmpty()));
    channelRail_->setVisible(mailboxState && channelPage);
    heading_->setVisible(!channelPage);
    channelRail_->setEnabled(session_.unlocked());
    search_->setPlaceholderText(channelPage ? "Search this channel" : "Search this folder");
    updateListCount();
    const auto identities = session_.unlocked() ? session_.identities() : QVariantList();
    if (identities != channelIdentities_) {
        channelIdentities_ = identities;
        refreshChannels();
    }
    const bool identityPage = folders_->currentRow() == 8 && mailboxState;
    sidebarWidget_->setVisible(mailboxState);
    listColumn_->setVisible(mailboxState && !identityPage);
    welcomeStack_->setVisible(!mailboxState);
    if (!mailboxState) {
        if (session_.unlocked()) {
            welcomeStack_->setCurrentWidget(noMailboxPage_);
            refreshRecentMailboxes();
        } else {
            const bool justFoundTarget =
                targetVaultPath_.isEmpty() && !session_.vaultPath().isEmpty();
            if (justFoundTarget)
                targetVaultPath_ = session_.vaultPath();
            welcomeStack_->setCurrentWidget(lockedPage_);
            updateLockedScreen();
            if (justFoundTarget)
                // A remembered vault is found on startup without going through
                // showVaultPasswordFor() -- focus the password field here too, once
                // it's actually the visible page, so typing can start immediately.
                vaultPasswordField_->setFocus();
        }
    }
    subject_->setVisible(mailboxState && !identityPage);
    findChild<QScrollArea *>("subjectScroll")->setVisible(mailboxState && !identityPage);
    body_->setVisible(mailboxState && !identityPage);
    letterStripe_->setVisible(mailboxState && !identityPage);
    identities_->setVisible(identityPage);
    status_->setText(
        !session_.unlocked()
            ? "Vault locked" +
                  (targetVaultPath_.isEmpty()
                       ? QString()
                       : " · " + QFileInfo(targetVaultPath_).fileName()) +
                  " · keys not loaded"
        : !mailboxState
            ? "Vault unlocked · no mailbox loaded"
            : session_.status() + " · " + QString::number(session_.objectCount()) +
                  " cached objects · " +
                  QString::number(session_.cacheBytes() / 1048576.0, 'f', 1) + " MB\n" +
                  session_.activity());
}
void DesktopWindow::refreshChannels() {
    const auto entries = session_.channels();
    bool stillJoined = false;
    for (auto v : entries)
        stillJoined = stillJoined || v.toMap()["address"].toString() == activeChannelAddress_;
    if (!stillJoined)
        activeChannelAddress_ = entries.isEmpty() ? QString() : entries.first().toMap()["address"].toString();
    while (auto item = channelChipLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (auto v : entries) {
        auto item = v.toMap();
        auto chipAddress = item["address"].toString();
        auto label = item["label"].toString();
        auto chip = new QPushButton(label);
        chip->setObjectName("channelChip");
        chip->setCheckable(true);
        chip->setChecked(chipAddress == activeChannelAddress_);
        chip->setIcon(QIcon(identiconPixmap(chipAddress, 18)));
        chip->setIconSize(QSize(18, 18));
        if (session_.channelUnread(chipAddress)) {
            auto f = chip->font();
            f.setBold(true);
            chip->setFont(f);
        }
        chip->setStyleSheet("QPushButton{text-align:left;padding:8px 10px;border:0;"
                            "border-radius:6px;} QPushButton:checked{background:palette(highlight);"
                            "color:palette(highlighted-text);}");
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip("<pre>" + chipAddress.toHtmlEscaped() + "</pre>");
        connect(chip, &QPushButton::clicked, this, [this, chipAddress] {
            activeChannelAddress_ = chipAddress;
            session_.messageModel()->setChannel(chipAddress);
            updateState();
            QTimer::singleShot(0, this, [this] { refreshChannels(); });
        });
        channelChipLayout_->addWidget(chip);
    }
    channelChipLayout_->addStretch();
    session_.messageModel()->setChannel(activeChannelAddress_);
    if (folders_->currentRow() == 4)
        heading_->setText("Channels");
}
void DesktopWindow::setListDensity(QString density) {
    if (density == listDensity_)
        return;
    listDensity_ = density;
    QSettings().setValue("listDensity", density);
    delete letterDelegate_;
    letterDelegate_ = new LetterDelegate(letters_, density);
    letters_->setItemDelegate(letterDelegate_);
}
void DesktopWindow::updateListCount() {
    auto model = session_.messageModel();
    const int shown = model->rowCount();
    const int total = model->totalCount();
    listCountLabel_->setText(shown == total
                                  ? QString::number(total) + " total"
                                  : QString::number(shown) + " of " + QString::number(total) +
                                        " total");
}
void DesktopWindow::selectMessage(const QString &id) {
    try {
        selected_ = session_.message(id);
    } catch (const std::exception &e) {
        error_->setText(e.what());
        error_->show();
        return;
    }
    static_cast<KindFrame *>(letterStripe_)
        ->setKind(classifyLetter(selected_["folder"].toString(), selected_["from"].toString(),
                                 selected_["to"].toString()));
    fromAddress_->setText(selected_["from"].toString());
    toAddress_->setText(selected_["to"].toString());
    deliveryError_->setText(selected_["deliveryError"].toString());
    deliveryError_->setVisible(!deliveryError_->text().isEmpty());
    updateDeliveryStatus();
    details_->show();
    updateTimeline();
    // Each letter opens in the mode its own content calls for; the switch is
    // then free for the reader to override on this letter.
    static_cast<ViewSwitch *>(viewSwitch_)
        ->setMode(detectBodyView(selected_["subject"].toString(), selected_["body"].toString()));
    renderSelectedBody();
    actions_->show();
    findChild<QAction *>("editAction")->setVisible(selected_["folder"] == "Drafts");
    findChild<QAction *>("replyAction")->setVisible(selected_["folder"] != "Drafts");
    const bool trashed = selected_["folder"] == "Trash";
    findChild<QAction *>("restoreAction")->setVisible(trashed);
    findChild<QAction *>("deletePermanentlyAction")->setVisible(trashed);
    const bool outgoing = selected_["folder"] == "Outbox";
    findChild<QAction *>("retryAction")->setVisible(outgoing);
    findChild<QAction *>("cancelDeliveryAction")->setVisible(outgoing);
    session_.readLetter(id);
}
void DesktopWindow::renderSelectedBody() {
    showLetterBody(body_, subject_, selected_["subject"].toString(), selected_["body"].toString(),
                   static_cast<ViewSwitch *>(viewSwitch_)->mode());
}
void DesktopWindow::compose(QVariantMap letter, bool reply) {
    if (!session_.mailboxOpen())
        return;
    if (letter.contains("hash"))
        letter = session_.message(letter["hash"].toString());
    Composer dialog(session_, letter, reply, appearance_.dark(), this);
    dialog.exec();
}
void DesktopWindow::showVaultPasswordFor(QString path, bool create) {
    targetVaultPath_ = path;
    vaultCreateMode_ = create;
    updateLockedScreen();
    vaultPasswordField_->setFocus();
}
void DesktopWindow::updateLockedScreen() {
    const bool hasTarget = !targetVaultPath_.isEmpty();
    lockedVaultName_->setText(QFileInfo(targetVaultPath_).fileName());
    lockedVaultPath_->setText(targetVaultPath_);
    findChild<QWidget *>("vaultAuthGroup")->setVisible(hasTarget);
    vaultRepeatField_->setVisible(vaultCreateMode_);
    vaultUnlockButton_->setText(vaultCreateMode_ ? "Create vault" : "Unlock vault");
    refreshRecentVaults();
}
void DesktopWindow::refreshRecentVaults() {
    while (auto item = lockedRecentsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (auto v : session_.recentVaults()) {
        auto m = v.toMap();
        auto path = m["path"].toString();
        auto row = button(m["name"].toString(), lockedRecentsLayout_,
                          [this, path] { showVaultPasswordFor(path, false); });
        row->setObjectName("recentVaultRow");
        row->setToolTip(path);
    }
}
void DesktopWindow::refreshRecentMailboxes() {
    while (auto item = noMailboxRecentsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (auto v : session_.recentMailboxes()) {
        auto m = v.toMap();
        auto path = m["path"].toString();
        auto row = button(m["name"].toString(), noMailboxRecentsLayout_,
                          [this, path] { session_.openMailboxAt(path); });
        row->setObjectName("recentMailboxRow");
        row->setToolTip(path);
    }
}
void DesktopWindow::refreshIdentities() {
    while (auto item = identityLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto color = iconColor(appearance_.dark());
    for (auto v : session_.identities()) {
        auto m = v.toMap();
        auto address = m["address"].toString();
        auto label = m["label"].toString();
        bool isDefault = m["default"].toBool();

        auto card = new QFrame;
        card->setObjectName("identityCard");
        card->setStyleSheet(
            "QFrame#identityCard{border:1px solid palette(mid);border-radius:8px;}");
        auto cardRow = new QHBoxLayout(card);
        cardRow->setContentsMargins(14, 12, 14, 12);
        cardRow->setSpacing(10);

        auto identicon = new QLabel;
        identicon->setObjectName("identityIdenticon");
        identicon->setFixedSize(40, 40);
        identicon->setPixmap(
            identiconPixmap(address, 40));
        cardRow->addWidget(identicon);

        auto infoCol = new QVBoxLayout;
        auto nameRow = new QHBoxLayout;
        auto nameLabel = new QLabel(label);
        nameLabel->setObjectName("identityName");
        nameLabel->setStyleSheet("font-weight:700;");
        nameLabel->setTextFormat(Qt::PlainText);
        if (label.contains("BM-"))
            nameLabel->setFont(addressFont());
        nameRow->addWidget(nameLabel);
        if (isDefault) {
            auto badge = new QLabel("Default");
            badge->setObjectName("defaultBadge");
            badge->setStyleSheet("font-size:11px;font-weight:600;padding:2px 7px;"
                                 "border-radius:5px;background:palette(highlight);"
                                 "color:palette(highlighted-text);");
            nameRow->addWidget(badge);
        }
        if (m["chan"].toBool()) {
            auto chanBadge = new QLabel("Channel");
            chanBadge->setObjectName("channelBadge");
            chanBadge->setStyleSheet("font-size:11px;font-weight:600;padding:2px 7px;"
                                     "border-radius:5px;background:palette(mid);");
            nameRow->addWidget(chanBadge);
        }
        nameRow->addStretch();
        infoCol->addLayout(nameRow);

        auto addressRow = new QHBoxLayout;
        auto addressLabel = new QLabel(address);
        addressLabel->setObjectName("identityAddress");
        addressLabel->setFont(addressFont());
        addressLabel->setStyleSheet("color:palette(mid);font-size:12px;");
        addressLabel->setTextFormat(Qt::PlainText);
        addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        addressRow->addWidget(addressLabel);
        auto copyButton = new QToolButton;
        copyButton->setObjectName("copyAddressButton");
        copyButton->setIcon(materialIcon("copy", color));
        copyButton->setIconSize(QSize(14, 14));
        copyButton->setAutoRaise(true);
        copyButton->setCursor(Qt::PointingHandCursor);
        copyButton->setToolTip("Copy address");
        connect(copyButton, &QToolButton::clicked, this,
                [this, address] { session_.copyAddress(address); });
        addressRow->addWidget(copyButton);
        addressRow->addStretch();
        infoCol->addLayout(addressRow);
        cardRow->addLayout(infoCol, 1);

        if (!isDefault) {
            auto setDefaultButton = new QPushButton("Set as default");
            setDefaultButton->setObjectName("setDefaultButton");
            setDefaultButton->setCursor(Qt::PointingHandCursor);
            connect(setDefaultButton, &QPushButton::clicked, this, [this, address] {
                session_.setDefaultIdentity(address);
                QTimer::singleShot(0, this, [this] { refreshIdentities(); });
            });
            cardRow->addWidget(setDefaultButton);
        }

        auto qrButton = new QToolButton;
        qrButton->setObjectName("showQrButton");
        qrButton->setIcon(materialIcon("qr", color));
        qrButton->setAutoRaise(true);
        qrButton->setCursor(Qt::PointingHandCursor);
        qrButton->setToolTip("Show QR code");
        connect(qrButton, &QToolButton::clicked, this, [this, address, label] {
            QDialog dialog(this);
            dialog.setObjectName("qrDialog");
            dialog.setWindowTitle(label);
            auto qrLayout = new QVBoxLayout(&dialog);
            qrLayout->addWidget(new QrCodeView(address, &dialog), 0, Qt::AlignHCenter);
            auto addressText = new QLabel(address);
            addressText->setFont(addressFont());
            addressText->setWordWrap(true);
            addressText->setAlignment(Qt::AlignHCenter);
            addressText->setTextFormat(Qt::PlainText);
            addressText->setTextInteractionFlags(Qt::TextSelectableByMouse);
            qrLayout->addWidget(addressText);
            auto buttonsRow = new QHBoxLayout;
            button("Copy address", buttonsRow, [this, address] { session_.copyAddress(address); });
            auto closeButton = new QPushButton("Close");
            closeButton->setObjectName("qrCloseButton");
            connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
            buttonsRow->addWidget(closeButton);
            qrLayout->addLayout(buttonsRow);
            dialog.exec();
        });
        cardRow->addWidget(qrButton);

        auto renameButton = new QToolButton;
        renameButton->setObjectName("renameIdentityButton");
        renameButton->setIcon(materialIcon("edit", color));
        renameButton->setAutoRaise(true);
        renameButton->setCursor(Qt::PointingHandCursor);
        renameButton->setToolTip("Rename");
        connect(renameButton, &QToolButton::clicked, this, [this, address] {
            session_.renameIdentity(address);
            QTimer::singleShot(0, this, [this] { refreshIdentities(); });
        });
        cardRow->addWidget(renameButton);

        auto deleteButton = new QToolButton;
        deleteButton->setObjectName("deleteIdentityButton");
        deleteButton->setIcon(materialIcon("delete", color));
        deleteButton->setAutoRaise(true);
        deleteButton->setCursor(Qt::PointingHandCursor);
        deleteButton->setToolTip("Delete");
        connect(deleteButton, &QToolButton::clicked, this, [this, address] {
            session_.deleteIdentity(address);
            QTimer::singleShot(0, this, [this] { refreshIdentities(); });
        });
        cardRow->addWidget(deleteButton);

        identityLayout_->addWidget(card);
    }
    identityLayout_->addStretch();
}
} // namespace bm
