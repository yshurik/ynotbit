#include "desktop_window.h"
#include "session.h"
#include "i18n.h"
#include "quoting.h"
#include "letter_document.h"
#include "letter_render.h"
#include "feed_view.h"
#include "updates.h"
#include "qrcodegen.hpp"
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFileInfo>
#include <QSettings>
#include <QTextList>
#include <QtWidgets>

namespace bm {
namespace {
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
// A delivery state code ("awaiting_ack") as the reader sees it. Unknown codes
// fall back to the code itself, spaced and capitalized.
QString deliveryStateLabel(const QString &state) {
    struct Label {
        const char *source, *comment;
    };
    // "delivery state" keeps these apart from same-looking words elsewhere,
    // e.g. the Sent folder.
    static const QHash<QString, Label> labels{
        {"queued", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Queued", "delivery state")},
        {"awaiting_pubkey", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Awaiting recipient key", "delivery state")},
        {"key_available", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Recipient key available", "delivery state")},
        {"calculating_ack", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Preparing receipt", "delivery state")},
        {"calculating_message", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Proof of work", "delivery state")},
        {"mining", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Proof of work", "delivery state")},
        {"prepared", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Prepared", "delivery state")},
        {"ready", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Ready", "delivery state")},
        {"publishing", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Submitting to relay", "delivery state")},
        {"offered", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Offered to peers", "delivery state")},
        {"published", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Published", "delivery state")},
        {"sent", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Sent", "delivery state")},
        {"awaiting_ack", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Awaiting acknowledgment", "delivery state")},
        {"acknowledged", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Acknowledged", "delivery state")},
        {"rejected", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Rejected", "delivery state")},
        {"failed", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Failed", "delivery state")},
        {"expired", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Expired", "delivery state")},
        {"cancelled", QT_TRANSLATE_NOOP3("bm::DesktopWindow", "Cancelled", "delivery state")},
    };
    if (labels.contains(state)) {
        const auto known = labels.value(state);
        return DesktopWindow::tr(known.source, known.comment);
    }
    auto label = state;
    label.replace('_', ' ');
    if (!label.isEmpty())
        label[0] = label[0].toUpper();
    return label;
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
        // The correspondent's local name (contact, subscription or own
        // identity), when there is one.
        // An anonymous chan post is "from" the chan itself, whose name the
        // chan page already shows.
        const bool anonymousPost = i.data(Qt::UserRole + 2) == i.data(Qt::UserRole + 3);
        const auto name =
            anonymousPost ? QString() : i.data(Qt::UserRole + (useRecipient ? 13 : 12)).toString();
        const auto withName = [&](const QString &line) {
            return name.isEmpty() ? line : line.isEmpty() ? name : name + " · " + line;
        };
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
            text(32, withName(previewLine), false, pal.placeholderText().color());
        } else {
            text(10, subject, unread, pal.text().color());
            text(36, previewLine, false, pal.placeholderText().color());
            auto state = i.data(Qt::UserRole + 7).toString();
            const auto stateColor =
                state == "acknowledged"
                    ? QColor(pal.base().color().lightness() < 128 ? "#8ce0b2" : "#17643b")
                    : pal.placeholderText().color();
            text(62,
                 withName(state.isEmpty() ? i.data(Qt::UserRole + 11).toString()
                                          : deliveryStateLabel(state)),
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
        setProperty("letterKind", int(kind));
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
// Folder names double as keys (English, untranslated); the markers let lupdate
// collect them, and the rail and heading translate them for display.
const QVector<QPair<QString, QString>> kFolderIcons = {
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Inbox"), "inbox"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Drafts"), "drafts"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Outbox"), "outbox"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Sent"), "sent"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Channels"), "channels"},
    {"Broadcasts", "broadcasts"}, // shown as "Subscriptions": see folderTitle()
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Archive"), "archive"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Trash"), "delete"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Identities"), "identities"},
    {QT_TRANSLATE_NOOP("bm::DesktopWindow", "Contacts"), "contacts"},
};
// A folder's displayed name. Received broadcasts are kept under "Broadcasts",
// but the page is the senders you follow: "Subscriptions", as in PyBitmessage.
QString folderTitle(const QString &key) {
    if (key == "Broadcasts")
        return DesktopWindow::tr("Subscriptions");
    return DesktopWindow::tr(key.toUtf8().constData());
}
// Paragraph types, as MarkText's "Turn into" menu offers them.
enum class ParagraphType { Paragraph, H1, H2, H3, H4, H5, H6, BulletList, NumberedList, Code };
ParagraphType paragraphType(const QTextBlock &block) {
    const auto format = block.blockFormat();
    if (format.hasProperty(QTextFormat::BlockCodeFence) ||
        format.hasProperty(QTextFormat::BlockCodeLanguage))
        return ParagraphType::Code;
    if (const int heading = format.headingLevel())
        return ParagraphType(qBound(1, heading, 6));
    if (auto list = block.textList())
        return list->format().style() == QTextListFormat::ListDecimal ? ParagraphType::NumberedList
                                                                      : ParagraphType::BulletList;
    return ParagraphType::Paragraph;
}
// The gutter mark for a type: ¶ for a paragraph, like MarkText.
QString paragraphSymbol(ParagraphType type) {
    switch (type) {
    case ParagraphType::Paragraph: return QString::fromUtf8("¶");
    case ParagraphType::BulletList: return QString::fromUtf8("•");
    case ParagraphType::NumberedList: return "1.";
    case ParagraphType::Code: return "</>";
    default: return "H" + QString::number(int(type));
    }
}
QString paragraphTypeName(ParagraphType type) {
    switch (type) {
    case ParagraphType::Paragraph: return DesktopWindow::tr("Paragraph");
    case ParagraphType::BulletList: return DesktopWindow::tr("Bullet list");
    case ParagraphType::NumberedList: return DesktopWindow::tr("Numbered list");
    case ParagraphType::Code: return DesktopWindow::tr("Code block");
    default: return DesktopWindow::tr("Heading %1").arg(int(type));
    }
}
// MarkText's keys (Linux/Windows): Ctrl+Shift+0..6, Ctrl+H, Ctrl+G, Ctrl+Shift+K.
QKeySequence paragraphShortcut(ParagraphType type) {
    switch (type) {
    case ParagraphType::Paragraph: return QKeySequence("Ctrl+Shift+0");
    case ParagraphType::BulletList: return QKeySequence("Ctrl+H");
    case ParagraphType::NumberedList: return QKeySequence("Ctrl+G");
    case ParagraphType::Code: return QKeySequence("Ctrl+Shift+K");
    default: return QKeySequence("Ctrl+Shift+" + QString::number(int(type)));
    }
}
QTextCharFormat headingCharFormat(int level) {
    QTextCharFormat t;
    t.setFontWeight(QFont::Bold);
    t.setFontPointSize(level == 1 ? 22 : level == 2 ? 18 : 15);
    return t;
}
// Turns the cursor's paragraph into another type. Its quote level stays: a
// quoted paragraph can become a heading or a list item, but stays quoted.
void setParagraphType(QTextCursor cursor, ParagraphType type) {
    auto block = cursor.block();
    const int level = blockQuoteLevel(block);
    const auto was = paragraphType(block);
    if (was == type)
        return;
    cursor.beginEditBlock();
    if (auto list = block.textList()) {
        list->remove(block);
        auto format = cursor.blockFormat();
        format.setIndent(0);
        cursor.setBlockFormat(format);
    }
    auto format = cursor.blockFormat();
    format.setHeadingLevel(0);
    format.clearProperty(QTextFormat::BlockCodeFence);
    format.clearProperty(QTextFormat::BlockCodeLanguage);
    cursor.setBlockFormat(format);
    QTextCursor text(block);
    text.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    const auto body = block.document()->defaultFont();
    if (was >= ParagraphType::H1 && was <= ParagraphType::H6) {
        QTextCharFormat plain;
        plain.setFontWeight(QFont::Normal);
        plain.setFontPointSize(body.pointSizeF() > 0 ? body.pointSizeF() : 10);
        plain.setProperty(QTextFormat::FontSizeAdjustment, 0);
        text.mergeCharFormat(plain);
    } else if (was == ParagraphType::Code) {
        QTextCharFormat plain;
        plain.setFontFamilies(QStringList{body.family()});
        plain.setFontFixedPitch(false);
        text.mergeCharFormat(plain);
    }
    if (type >= ParagraphType::H1 && type <= ParagraphType::H6) {
        format = cursor.blockFormat();
        format.setHeadingLevel(int(type));
        cursor.setBlockFormat(format);
        text.mergeCharFormat(headingCharFormat(int(type)));
    } else if (type == ParagraphType::BulletList || type == ParagraphType::NumberedList) {
        QTextListFormat list;
        list.setStyle(type == ParagraphType::BulletList ? QTextListFormat::ListDisc
                                                        : QTextListFormat::ListDecimal);
        cursor.createList(list);
    } else if (type == ParagraphType::Code) {
        format = cursor.blockFormat();
        format.setProperty(QTextFormat::BlockCodeFence, QChar('`'));
        cursor.setBlockFormat(format);
        QTextCharFormat code;
        code.setFontFamilies(addressFont().families());
        code.setFontFixedPitch(true);
        text.mergeCharFormat(code);
    }
    setQuoteLevel(cursor.block(), level);
    cursor.endEditBlock();
}
class MarkdownEdit : public QTextEdit {
  public:
    explicit MarkdownEdit(QWidget *parent = nullptr) : QTextEdit(parent) {
        for (auto type : {ParagraphType::Paragraph, ParagraphType::H1, ParagraphType::H2,
                          ParagraphType::H3, ParagraphType::H4, ParagraphType::H5,
                          ParagraphType::H6, ParagraphType::BulletList,
                          ParagraphType::NumberedList, ParagraphType::Code})
            shortcut(paragraphShortcut(type), [this, type] { turnInto(type); });
        shortcut(QKeySequence("Ctrl+Shift+E"), [this] { duplicateParagraph(); });
        shortcut(QKeySequence("Ctrl+Shift+N"), [this] { newParagraph(); });
        shortcut(QKeySequence("Ctrl+Shift+D"), [this] { deleteParagraph(); });
    }
    void turnInto(ParagraphType type) {
        setParagraphType(textCursor(), type);
    }
    void duplicateParagraph() {
        const auto block = textCursor().block();
        QTextCursor source(block);
        source.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        const auto contents = source.selection();
        QTextCursor c(block);
        c.beginEditBlock();
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertBlock(block.blockFormat(), block.charFormat());
        c.insertFragment(contents);
        if (auto list = block.textList())
            list->add(c.block());
        c.endEditBlock();
        setTextCursor(c);
    }
    // A new, unquoted paragraph below this one: the way to answer inline,
    // between quoted paragraphs.
    void newParagraph() {
        auto c = textCursor();
        c.beginEditBlock();
        c.movePosition(QTextCursor::EndOfBlock);
        QTextBlockFormat plain;
        plain.setBottomMargin(c.blockFormat().bottomMargin());
        c.insertBlock(plain, QTextCharFormat());
        setQuoteLevel(c.block(), 0);
        c.endEditBlock();
        setTextCursor(c);
    }
    void deleteParagraph() {
        auto c = textCursor();
        c.beginEditBlock();
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        if (!c.atEnd())
            c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        else if (c.block().previous().isValid()) {
            // The last paragraph: take the break before it instead.
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::PreviousCharacter);
            c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        }
        c.removeSelectedText();
        c.endEditBlock();
        setTextCursor(c);
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QTextEdit::paintEvent(event);
        paintQuoteBars(this);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Space && tryAutoFormat())
            return;
        if (keepQuotes(event))
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
    void shortcut(const QKeySequence &keys, std::function<void()> fn) {
        auto s = new QShortcut(keys, this);
        s->setContext(Qt::WidgetShortcut);
        connect(s, &QShortcut::activated, this, std::move(fn));
    }
    // Quoted paragraphs keep their quote: text can be edited or deleted, but
    // a quoted paragraph never merges into an unquoted one (or the reverse),
    // which is how Backspace or Delete at a boundary would drop the quote.
    // Enter on an empty quoted line leaves the quote, to answer inline.
    bool keepQuotes(QKeyEvent *event) {
        auto c = textCursor();
        if (c.hasSelection())
            return false;
        const auto block = c.block();
        const int level = blockQuoteLevel(block);
        const bool empty = block.text().isEmpty();
        if (event->key() == Qt::Key_Backspace && c.atBlockStart()) {
            const auto previous = block.previous();
            if (previous.isValid() && blockQuoteLevel(previous) != level && !empty)
                return true;
        } else if (event->key() == Qt::Key_Delete && c.atBlockEnd()) {
            const auto next = block.next();
            if (next.isValid() && blockQuoteLevel(next) != level) {
                if (!empty || next.text().isEmpty())
                    return true;
                // An empty line before a quote: remove it, not the quote.
                c.beginEditBlock();
                c.setBlockFormat(next.blockFormat());
                c.deleteChar();
                c.endEditBlock();
                return true;
            }
        } else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                   level > 0 && empty && !(event->modifiers() & Qt::ShiftModifier)) {
            setQuoteLevel(block, 0);
            return true;
        }
        return false;
    }
    void insertMarkdown(const QString &text) {
        QTextDocument doc;
        loadLetter(&doc, text, true);
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
        if (prefix == "#" || prefix == "##" || prefix == "###")
            return apply([&](QTextCursor &c) { setParagraphType(c, ParagraphType(prefix.size())); });
        if (prefix == "-" || prefix == "*")
            return apply([](QTextCursor &c) { setParagraphType(c, ParagraphType::BulletList); });
        if (prefix == "1.")
            return apply([](QTextCursor &c) { setParagraphType(c, ParagraphType::NumberedList); });
        if (prefix == ">")
            return apply([](QTextCursor &c) { setQuoteLevel(c.block(), blockQuoteLevel(c.block()) + 1); });
        return false;
    }
};
// The paragraph gutter, after MarkText: the paragraph under the mouse (or
// holding the cursor) shows its type -- ¶, H1..H6, a list or code mark --
// and clicking it opens "Turn into" and the paragraph actions. Headings
// always show their level.
class HeadingGutter : public QWidget {
  public:
    explicit HeadingGutter(MarkdownEdit *editor) : editor_(editor) {
        setFixedWidth(34);
        setMouseTracking(true);
        setToolTip(DesktopWindow::tr("Paragraph type"));
        connect(editor_->document(), &QTextDocument::contentsChanged, this, [this] { update(); });
        connect(editor_, &QTextEdit::cursorPositionChanged, this, [this] { update(); });
        connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { update(); });
    }
    // The menu for a paragraph, as the gutter shows it.
    QMenu *menuFor(QTextBlock block) {
        auto cursor = editor_->textCursor();
        if (cursor.block() != block) {
            cursor.setPosition(block.position());
            editor_->setTextCursor(cursor);
        }
        auto menu = new QMenu(this);
        menu->setObjectName("paragraphMenu");
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addSection(DesktopWindow::tr("Turn into"));
        const auto current = paragraphType(block);
        for (auto type : {ParagraphType::Paragraph, ParagraphType::H1, ParagraphType::H2,
                          ParagraphType::H3, ParagraphType::H4, ParagraphType::H5,
                          ParagraphType::H6, ParagraphType::BulletList,
                          ParagraphType::NumberedList, ParagraphType::Code}) {
            auto action = menu->addAction(paragraphSymbol(type) + "   " + paragraphTypeName(type),
                                          editor_, [this, type] { editor_->turnInto(type); });
            action->setObjectName(QString("turnInto_%1").arg(int(type)));
            // Only the current type carries a mark, as in MarkText.
            action->setCheckable(type == current);
            action->setChecked(type == current);
            action->setShortcut(paragraphShortcut(type));
            // Shown as a hint only: the editor owns the key itself.
            action->setShortcutContext(Qt::WidgetShortcut);
            action->setShortcutVisibleInContextMenu(true);
        }
        menu->addSeparator();
        const struct {
            QString name, text;
            QKeySequence keys;
            void (MarkdownEdit::*act)();
        } actions[] = {
            {"duplicateParagraph", DesktopWindow::tr("Duplicate"), QKeySequence("Ctrl+Shift+E"),
             &MarkdownEdit::duplicateParagraph},
            {"newParagraph", DesktopWindow::tr("New paragraph below"), QKeySequence("Ctrl+Shift+N"),
             &MarkdownEdit::newParagraph},
            {"deleteParagraph", DesktopWindow::tr("Delete paragraph"), QKeySequence("Ctrl+Shift+D"),
             &MarkdownEdit::deleteParagraph},
        };
        for (const auto &a : actions) {
            auto action = menu->addAction(a.text, editor_, [this, act = a.act] { (editor_->*act)(); });
            action->setObjectName(a.name);
            action->setShortcut(a.keys);
            action->setShortcutContext(Qt::WidgetShortcut);
            action->setShortcutVisibleInContextMenu(true);
        }
        return menu;
    }
    QRect markRect(const QTextBlock &block) const {
        const auto rect = editor_->document()->documentLayout()->blockBoundingRect(block);
        const int top = int(rect.top()) - editor_->verticalScrollBar()->value() +
                        editor_->viewport()->y();
        return QRect(2, top, width() - 6, qMax(18, qMin(int(rect.height()), 26)));
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto f = p.font();
        f.setPointSize(9);
        f.setBold(true);
        p.setFont(f);
        const auto current = editor_->textCursor().block();
        auto doc = editor_->document();
        for (auto block = doc->begin(); block.isValid(); block = block.next()) {
            const auto rect = markRect(block);
            if (rect.bottom() < 0 || rect.top() > height())
                continue;
            const bool active = block == current || block == hovered_;
            const auto type = paragraphType(block);
            if (!active && (type < ParagraphType::H1 || type > ParagraphType::H6))
                continue;
            if (block == hovered_) {
                auto fill = palette().color(QPalette::Highlight);
                fill.setAlpha(60);
                p.setPen(Qt::NoPen);
                p.setBrush(fill);
                p.drawRoundedRect(rect, 4, 4);
            }
            p.setPen(palette().color(active ? QPalette::Text : QPalette::PlaceholderText));
            p.drawText(rect, Qt::AlignCenter, paragraphSymbol(type));
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        const auto block = blockAt(event->position().y());
        if (block != hovered_) {
            hovered_ = block;
            setCursor(block.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    void leaveEvent(QEvent *) override {
        hovered_ = QTextBlock();
        update();
    }
    void mousePressEvent(QMouseEvent *event) override {
        const auto block = blockAt(event->position().y());
        if (!block.isValid())
            return;
        auto menu = menuFor(block);
        menu->popup(mapToGlobal(markRect(block).bottomLeft()));
    }

  private:
    QTextBlock blockAt(qreal y) const {
        for (auto block = editor_->document()->begin(); block.isValid(); block = block.next()) {
            const auto rect = editor_->document()->documentLayout()->blockBoundingRect(block);
            const qreal top = rect.top() - editor_->verticalScrollBar()->value() +
                              editor_->viewport()->y();
            if (y >= top && y < top + rect.height())
                return block;
        }
        return {};
    }
    MarkdownEdit *editor_;
    QTextBlock hovered_;
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
    MarkdownEdit *body_;
    QLabel *status_, *sizeLabel_;
    QProgressBar *sizeBar_;
    QPushButton *send_;
    QTimer autosave_, measure_;
    bool dirty_ = false, bodyEdited_ = false;
    QString bodyText() const {
        // ynotbit's own Markdown writer: Qt's export drops quoting from
        // headings and joins the line after a list into the list.
        return bodyEdited_ ? letterMarkdown(body_->document()) : original_;
    }
    // The letter's size against the most one Bitmessage object carries; a
    // letter over it can still be kept as a draft, but not sent.
    void measure() {
        const int size = letterTextBytes(subject_->text(), bodyText());
        const auto amount = [](qint64 bytes) {
            return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
        };
        const bool over = size > kMaxLetterText;
        const bool near = size > kMaxLetterText * 9 / 10;
        sizeBar_->setValue(int(qMin<qint64>(size, kMaxLetterText) * 1000 / kMaxLetterText));
        sizeLabel_->setText(DesktopWindow::tr("%1 of %2").arg(amount(size), amount(kMaxLetterText)));
        const QString color = over ? "#d64545" : near ? "#c98a1e" : "#7c8b96";
        sizeBar_->setStyleSheet(QString("QProgressBar{border:0;border-radius:2px;background:rgba(124,139,150,0.25);}"
                                        "QProgressBar::chunk{border-radius:2px;background:%1;}")
                                    .arg(color));
        sizeLabel_->setStyleSheet(over || near ? QString("color:%1;").arg(color) : QString());
        sizeBar_->setProperty("over", over);
        send_->setEnabled(!over);
        send_->setDefault(!over); // disabling a default button drops its default
        send_->setToolTip(over ? DesktopWindow::tr("Too large to send: Bitmessage carries at most %1 "
                                                   "per letter. Shorten it, or trim the quote.")
                                     .arg(amount(kMaxLetterText))
                               : QString());
    }
    bool save() {
        if (!dirty_)
            return true;
        auto body = bodyText();
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
        setWindowIcon(windowLogo());
        setWindowTitle(reply ? DesktopWindow::tr("Reply") : DesktopWindow::tr("Write a letter"));
        resize(740, 650);
        setModal(true);
        // The letter's envelope: the same striped border as the reader, for
        // the kind of letter this will be once sent.
        auto outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        auto kindFrame = new KindFrame;
        kindFrame->setObjectName("composerKindStripe");
        outer->addWidget(kindFrame);
        auto layout = new QVBoxLayout;
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(12);
        kindFrame->contentLayout()->addLayout(layout);
        // Private/Public is one choice: a segmented switch (styled with the
        // app's other switches), its explanation beside it.
        auto modeRow = new QHBoxLayout;
        modeRow->setSpacing(14);
        auto modeSwitch = new QWidget;
        modeSwitch->setObjectName("modeSwitch");
        auto switchLayout = new QHBoxLayout(modeSwitch);
        switchLayout->setContentsMargins(0, 0, 0, 0);
        switchLayout->setSpacing(0);
        modePrivate_ = new QPushButton;
        modePrivate_->setObjectName("modePrivateButton");
        modePrivate_->setCheckable(true);
        modePrivate_->setChecked(true);
        modePublic_ = new QPushButton;
        modePublic_->setObjectName("modePublicButton");
        modePublic_->setCheckable(true);
        auto modeGroup = new QButtonGroup(this);
        modeGroup->setExclusive(true);
        modeGroup->addButton(modePrivate_);
        modeGroup->addButton(modePublic_);
        switchLayout->addWidget(modePrivate_);
        switchLayout->addWidget(modePublic_);
        modeRow->addWidget(modeSwitch, 0, Qt::AlignVCenter);
        auto modeHint = new QLabel;
        modeHint->setObjectName("modeHint");
        modeHint->setWordWrap(true);
        {
            auto f = modeHint->font();
            f.setPointSizeF(f.pointSizeF() * 0.9);
            modeHint->setFont(f);
        }
        modeRow->addWidget(modeHint, 1, Qt::AlignVCenter);
        layout->addLayout(modeRow);
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
        // From, To and Subject, each named on its left.
        auto fields = new QGridLayout;
        fields->setHorizontalSpacing(10);
        fields->setVerticalSpacing(8);
        fields->setColumnStretch(1, 1);
        auto fieldLabel = [](const QString &text, QWidget *buddy) {
            auto label = new QLabel(text);
            label->setBuddy(buddy);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            return label;
        };
        fields->addWidget(fieldLabel(DesktopWindow::tr("From:"), sender_), 0, 0);
        fields->addWidget(sender_, 0, 1);
        layout->addLayout(fields);
        auto updateModeLabels = [this, identities, modeHint, kindFrame] {
            bool channel = false;
            for (auto value : identities) {
                auto identity = value.toMap();
                if (identity["address"] == sender_->currentData()) {
                    channel = identity["chan"].toBool();
                    break;
                }
            }
            const bool privately = modePrivate_->isChecked();
            kindFrame->setKind(channel ? (privately ? LetterKind::ChanPersonal : LetterKind::ChanAnonymous)
                                       : (privately ? LetterKind::Personal : LetterKind::Broadcast));
            modePrivate_->setText(channel ? DesktopWindow::tr("Personal") : DesktopWindow::tr("Private mail"));
            modePublic_->setText(channel ? DesktopWindow::tr("Anonymous") : DesktopWindow::tr("Public mail"));
            if (modePrivate_->isChecked())
                modeHint->setText(channel
                                       ? DesktopWindow::tr("Encrypted to the channel's shared address. Anyone who "
                                         "knows the channel phrase can read it.")
                                       : DesktopWindow::tr("Encrypted to one recipient. Only they can read it."));
            else
                modeHint->setText(
                    channel
                        ? DesktopWindow::tr("Sent as the channel to everyone subscribed. Your own identity isn't "
                          "revealed.")
                        : DesktopWindow::tr("Sent to everyone subscribed to your address. Anyone can read it."));
        };
        updateModeLabels();
        connect(sender_, &QComboBox::currentIndexChanged, this, updateModeLabels);
        connect(modePrivate_, &QPushButton::toggled, this, updateModeLabels);
        modePublic_->setChecked(!reply && letter["kind"] == "broadcast");
        modePrivate_->setChecked(reply || letter["kind"] != "broadcast");
        to_ = new QLineEdit;
        to_->setFont(addressFont());
        to_->setObjectName("recipientField");
        to_->setPlaceholderText(DesktopWindow::tr("Recipient · BM-address"));
        to_->setText(reply ? letter[letter["folder"] == "Channels" ? "to" : "from"].toString()
                           : letter["to"].toString());
        // The address book, three ways: completion on name or address, a
        // picker listing every contact, and a line naming the recipient.
        const auto contacts = session.contacts();
        auto completions = new QStandardItemModel(this);
        auto picker = new QMenu(this);
        picker->setObjectName("contactsPickerMenu");
        for (auto v : contacts) {
            const auto c = v.toMap();
            const auto address = c["address"].toString(), label = c["label"].toString();
            auto item = new QStandardItem(label + " — " + address);
            item->setData(address, Qt::UserRole);
            completions->appendRow(item);
            picker->addAction(QIcon(identiconPixmap(address, 18)), label, this,
                              [this, address] { to_->setText(address); });
        }
        auto completer = new QCompleter(completions, this);
        completer->setObjectName("recipientCompleter");
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        to_->setCompleter(completer);
        // Choosing "Name — BM-…" leaves just the address in the field.
        connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated), this,
                [this](const QModelIndex &index) {
                    const auto address = index.data(Qt::UserRole).toString();
                    QTimer::singleShot(0, this, [this, address] { to_->setText(address); });
                });
        auto toLabel = fieldLabel(DesktopWindow::tr("To:"), to_);
        toLabel->setObjectName("recipientLabel");
        fields->addWidget(toLabel, 1, 0);
        auto toRow = new QHBoxLayout;
        toRow->setSpacing(6);
        toRow->addWidget(to_, 1);
        fields->addLayout(toRow, 1, 1);
        auto pickerButton = new QPushButton;
        pickerButton->setObjectName("contactsPickerButton");
        pickerButton->setIcon(materialIcon("contacts", iconColor(dark)));
        pickerButton->setIconSize(QSize(18, 18));
        pickerButton->setToolTip(DesktopWindow::tr("Choose from contacts"));
        pickerButton->setMenu(picker);
        pickerButton->setVisible(!contacts.isEmpty());
        toRow->addWidget(pickerButton);
        subject_ = new QLineEdit;
        subject_->setObjectName("subjectField");
        subject_->setPlaceholderText(DesktopWindow::tr("Subject"));
        auto subject = singleLine(letter["subject"].toString());
        if (reply && !subject.startsWith("Re:", Qt::CaseInsensitive))
            subject.prepend("Re: ");
        subject_->setText(subject);
        fields->addWidget(fieldLabel(DesktopWindow::tr("Subject:"), subject_), 2, 0);
        fields->addWidget(subject_, 2, 1);
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
        if (reply) {
            // Email style: room to write at the top, the signature, then the
            // letter being answered, quoted with ">" under an attribution --
            // all in this one editor. Quoted paragraphs can be edited but keep
            // their quote (see MarkdownEdit).
            const auto from = letter["from"].toString();
            const auto name = session_.nameFor(from);
            const auto when = QDateTime::fromSecsSinceEpoch(letter["storedAt"].toLongLong());
            original_ += "\n\n" +
                         quoteForReply(letter["body"].toString(),
                                       DesktopWindow::tr("On %1, %2 wrote:")
                                           .arg(formatDateTime(when), name.isEmpty() ? from : name));
        }
        loadLetter(doc, original_, true);
        // Paragraphs are blocks, not blank lines: give them room to breathe,
        // as the reader does. New paragraphs inherit it.
        for (auto block = doc->begin(); block.isValid(); block = block.next()) {
            auto format = block.blockFormat();
            format.setBottomMargin(block.textList() ? 2 : 8);
            QTextCursor(block).setBlockFormat(format);
        }
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
        body_->setPlaceholderText(DesktopWindow::tr("Take your time. Write something worth sending."));
        auto bodyRow = new QHBoxLayout;
        bodyRow->setContentsMargins(0, 0, 0, 0);
        bodyRow->setSpacing(4);
        auto gutter = new HeadingGutter(body_);
        gutter->setObjectName("headingGutter");
        bodyRow->addWidget(gutter);
        bodyRow->addWidget(body_, 1);
        layout->addLayout(bodyRow, 1);
        auto icon = [dark](QString name) { return materialIcon(name, iconColor(dark)); };
        // Formatting lives on the toolbar that floats over a selection; the
        // actions sit on the editor so their shortcuts work without it.
        auto format = [&](QString iconName, QString objName, QString tip,
                          std::function<void()> fn) {
            auto a = new QAction(icon(iconName), tip, body_);
            a->setObjectName(objName);
            a->setShortcutContext(Qt::WidgetShortcut);
            body_->addAction(a);
            connect(a, &QAction::triggered, this, [this, fn] {
                fn();
                body_->setFocus();
            });
            return a;
        };
        auto bold = format("bold", "boldAction", DesktopWindow::tr("Bold"), [this] {
            QTextCharFormat f;
            f.setFontWeight(body_->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
            body_->mergeCurrentCharFormat(f);
        });
        bold->setShortcut(QKeySequence::Bold);
        auto italic = format("italic", "italicAction", DesktopWindow::tr("Italic"), [this] {
            QTextCharFormat f;
            f.setFontItalic(!body_->fontItalic());
            body_->mergeCurrentCharFormat(f);
        });
        italic->setShortcut(QKeySequence::Italic);
        auto strike = format("strike", "strikeAction", DesktopWindow::tr("Strikethrough"), [this] {
            QTextCharFormat f;
            f.setFontStrikeOut(!body_->currentCharFormat().fontStrikeOut());
            body_->mergeCurrentCharFormat(f);
        });
        auto code = format("code", "codeAction", DesktopWindow::tr("Inline code"), [this] {
            bool isCode = body_->currentCharFormat().fontFixedPitch();
            QTextCharFormat f;
            f.setFontFixedPitch(!isCode);
            if (!isCode)
                f.setFontFamilies({"monospace"});
            body_->mergeCurrentCharFormat(f);
        });
        auto link = format("link", "linkAction", DesktopWindow::tr("Link"), [this] {
            bool ok;
            auto url = QInputDialog::getText(this, DesktopWindow::tr("Insert link"), DesktopWindow::tr("https:// address"),
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
        auto clear = format("eraser", "clearFormatAction", DesktopWindow::tr("Clear formatting"), [this] {
            auto c = body_->textCursor();
            if (c.hasSelection())
                c.setCharFormat(QTextCharFormat());
            else
                body_->setCurrentCharFormat(QTextCharFormat());
        });
        floatingToolbar_ = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        floatingToolbar_->setObjectName("floatingToolbar");
        floatingToolbar_->setAttribute(Qt::WA_ShowWithoutActivating);
        auto floatLayout = new QHBoxLayout(floatingToolbar_);
        floatLayout->setContentsMargins(4, 4, 4, 4);
        floatLayout->setSpacing(2);
        for (auto [action, name] : {std::pair{bold, "floatBoldButton"}, {italic, "floatItalicButton"},
                                    {strike, "floatStrikeButton"}, {code, "floatCodeButton"},
                                    {link, "floatLinkButton"}, {clear, "floatClearButton"}}) {
            auto b = new QToolButton;
            b->setObjectName(name);
            b->setDefaultAction(action);
            b->setIconSize(QSize(20, 20));
            floatLayout->addWidget(b);
        }
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
        auto discard = button(DesktopWindow::tr("Discard draft"), actions, [this] {
            if (!id_.isEmpty())
                session_.moveLetter(id_, "Trash");
            dirty_ = false;
            QDialog::done(QDialog::Rejected);
        });
        discard->setObjectName("discardButton");
        actions->addStretch();
        // The size meter, beside the buttons it decides about.
        auto meter = new QWidget;
        meter->setObjectName("sizeMeter");
        meter->setToolTip(DesktopWindow::tr("The letter's size, out of the most one Bitmessage object "
                                            "carries. Larger letters also take longer to prepare "
                                            "(proof of work)."));
        auto meterLayout = new QVBoxLayout(meter);
        meterLayout->setContentsMargins(0, 0, 8, 0);
        meterLayout->setSpacing(3);
        sizeLabel_ = new QLabel;
        sizeLabel_->setObjectName("sizeLabel");
        {
            auto f = sizeLabel_->font();
            f.setPointSizeF(f.pointSizeF() * 0.85);
            sizeLabel_->setFont(f);
        }
        sizeLabel_->setAlignment(Qt::AlignRight);
        sizeBar_ = new QProgressBar;
        sizeBar_->setObjectName("sizeBar");
        sizeBar_->setRange(0, 1000);
        sizeBar_->setTextVisible(false);
        sizeBar_->setFixedSize(110, 4);
        meterLayout->addWidget(sizeLabel_, 0, Qt::AlignRight);
        meterLayout->addWidget(sizeBar_, 0, Qt::AlignRight);
        actions->addWidget(meter, 0, Qt::AlignVCenter);
        button(DesktopWindow::tr("Save a draft"), actions, [this] {
            dirty_ = true;
            if (save())
                accept();
        })->setObjectName("saveDraftButton");
        auto send = send_ = button(DesktopWindow::tr("Send"), actions, [this] {
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
        measure_.setSingleShot(true);
        connect(&measure_, &QTimer::timeout, this, [this] { measure(); });
        connect(body_, &QTextEdit::textChanged, this, [this, changed] {
            bodyEdited_ = true;
            changed();
            measure_.start(250);
        });
        connect(subject_, &QLineEdit::textChanged, this, [this] { measure_.start(250); });
        measure();
        connect(to_, &QLineEdit::textEdited, this, changed);
        connect(subject_, &QLineEdit::textEdited, this, changed);
        connect(sender_, &QComboBox::currentIndexChanged, this, changed);
        // Public mail has no recipient: the whole To row goes.
        auto showRecipient = [this, toLabel, pickerButton, hasContacts = !contacts.isEmpty()] {
            const bool shown = !modePublic_->isChecked();
            to_->setVisible(shown);
            toLabel->setVisible(shown);
            pickerButton->setVisible(shown && hasContacts);
        };
        connect(modePublic_, &QPushButton::toggled, this, [showRecipient, changed] {
            showRecipient();
            changed();
        });
        showRecipient();
        connect(&session_, &Session::aboutToCloseMailbox, this, [this] {
            if (save())
                accept();
        });
    }
    void reject() override {
        if (save()) {
            QDialog::reject();
            return;
        }
        // A draft that can't be saved must not trap its window.
        QMessageBox box(QMessageBox::Warning, windowTitle(),
                        DesktopWindow::tr("This draft couldn't be saved: %1").arg(session_.error()),
                        QMessageBox::NoButton, this);
        box.setObjectName("unsavedDraftBox");
        box.setInformativeText(
            DesktopWindow::tr("Close anyway? Changes since the last save will be lost."));
        auto close = box.addButton(DesktopWindow::tr("Close without saving"),
                                   QMessageBox::DestructiveRole);
        close->setObjectName("closeWithoutSavingButton");
        box.setDefaultButton(box.addButton(DesktopWindow::tr("Keep editing"), QMessageBox::RejectRole));
        box.exec();
        if (box.clickedButton() == close) {
            dirty_ = false;
            QDialog::reject();
        }
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
        // Hug the buttons: stretched by a taller row, the rounded border would
        // leave a gap around them.
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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
            btn->setToolTip(DesktopWindow::tr(kViews[i].label));
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
        {"plain", "viewPlain", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Plain text, fixed width")},
        {"text", "viewText", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Text")},
        {"markdown", "viewMarkdown", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Markdown")},
        {"hex", "viewHex", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Hex")},
    };
    QToolButton *buttons_[4];
};
// Shows a letter in the given mode. Plain and hex are the fixed-width modes,
// and the subject rides along: if a body needed a monospace grid to make
// sense, its subject does too.
void showLetterBody(QTextBrowser *body, QTextEdit *subject, const QString &subjectText,
                    const QString &text, BodyView mode) {
    renderBody(body, text, mode);
    setSubject(subject, subjectText, mode);
}
// Add or rename an address-book entry. With no address given, a valid one on
// the clipboard is offered; validation is live and Save only enables for an
// address the book will take.
// A column of fixed width whose minimum height is what its contents need at
// that width. Qt counts a word-wrapped label's minimum before knowing the
// width, so a label wrapping onto more lines (Windows' font, longer
// translations) took the room of the widgets below it.
class FixedWidthColumn : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    bool event(QEvent *event) override {
        const bool handled = QWidget::event(event);
        if ((event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize) &&
            layout())
            setMinimumHeight(layout()->totalHeightForWidth(width()));
        return handled;
    }
};
// One line, shortened in the middle to fit ("/Users/…/a.bmvault"), the whole
// text in its tooltip. A word-wrapped path made the box's height depend on
// where the lines happened to break, and long ones were cut off.
class ElidedLabel : public QLabel {
  public:
    using QLabel::QLabel;
    void setFullText(const QString &text) {
        full_ = text;
        setToolTip(text);
        elide();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        elide();
    }

  private:
    void elide() {
        QLabel::setText(fontMetrics().elidedText(full_, Qt::ElideMiddle, width()));
    }
    QString full_;
};
class ContactDialog : public QDialog {
  public:
    ContactDialog(Session &session, QString address, QString label, QWidget *parent)
        : QDialog(parent), session_(session) {
        setObjectName("contactDialog");
        setWindowTitle(label.isEmpty() ? DesktopWindow::tr("Add contact") : DesktopWindow::tr("Rename contact"));
        setMinimumWidth(460);
        if (address.isEmpty()) {
            const auto clip = QApplication::clipboard()->text().trimmed();
            if (session_.contactProblem(clip).isEmpty())
                address = clip;
        }
        auto layout = new QVBoxLayout(this);
        layout->setSpacing(8);
        layout->addWidget(new QLabel(DesktopWindow::tr("Name")));
        name_ = new QLineEdit(label);
        name_->setObjectName("contactNameField");
        name_->setPlaceholderText(DesktopWindow::tr("How this person appears in ynotbit"));
        layout->addWidget(name_);
        layout->addWidget(new QLabel(DesktopWindow::tr("Address")));
        address_ = new QLineEdit(address);
        address_->setObjectName("contactAddressField");
        address_->setFont(addressFont());
        address_->setPlaceholderText(DesktopWindow::tr("BM-…"));
        // A rename keeps the address; changing it would be a different contact.
        address_->setReadOnly(!label.isEmpty());
        layout->addWidget(address_);
        note_ = new QLabel;
        note_->setObjectName("contactProblem");
        note_->setWordWrap(true);
        note_->setStyleSheet("color:palette(mid);font-size:12px;");
        layout->addWidget(note_);
        auto private_ = new QLabel(DesktopWindow::tr("Names are private to this mailbox and never sent to anyone."));
        private_->setWordWrap(true);
        private_->setStyleSheet("color:palette(mid);font-size:11px;");
        layout->addWidget(private_);
        auto buttons = new QHBoxLayout;
        buttons->addStretch();
        auto cancel = new QPushButton(DesktopWindow::tr("Cancel"));
        cancel->setObjectName("contactCancelButton");
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(cancel);
        save_ = new QPushButton(DesktopWindow::tr("Save"));
        save_->setObjectName("contactSaveButton");
        save_->setDefault(true);
        connect(save_, &QPushButton::clicked, this, [this] {
            if (session_.addContact(address_->text(), name_->text()))
                accept();
            else
                note_->setText(session_.error());
        });
        buttons->addWidget(save_);
        layout->addLayout(buttons);
        connect(address_, &QLineEdit::textChanged, this, [this] { validate(); });
        validate();
        (address.isEmpty() ? address_ : name_)->setFocus();
    }

  private:
    void validate() {
        const auto address = address_->text().trimmed();
        const auto problem = address.isEmpty() ? QString() : session_.contactProblem(address);
        auto note = problem;
        if (problem.isEmpty() && !address_->isReadOnly() && session_.isContact(address))
            note = DesktopWindow::tr("Already a contact, as “%1”. Saving renames it.")
                       .arg(session_.nameFor(address));
        note_->setText(note);
        save_->setEnabled(!address.isEmpty() && problem.isEmpty());
    }
    Session &session_;
    QLineEdit *name_, *address_;
    QLabel *note_;
    QPushButton *save_;
};
class MessageWindow : public QDialog {
  public:
    MessageWindow(Session &session, QVariantMap letter, bool dark, QWidget *parent)
        : QDialog(parent), session_(session), letter_(std::move(letter)), dark_(dark) {
        setObjectName("messageWindow");
        setWindowIcon(windowLogo());
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
        auto replyAction = toolbar->addAction(materialIcon("reply", iconColor(dark)), DesktopWindow::tr("Reply"));
        connect(replyAction, &QAction::triggered, this, [this] {
            Composer dialog(session_, letter_, true, dark_, this);
            dialog.exec();
        });
        auto archiveAction = toolbar->addAction(materialIcon("archive", iconColor(dark)), DesktopWindow::tr("Archive"));
        connect(archiveAction, &QAction::triggered, this,
                [this] { session_.moveLetter(letter_["hash"].toString(), "Archive"); });
        auto trashAction = toolbar->addAction(materialIcon("delete", iconColor(dark)), DesktopWindow::tr("Trash"));
        connect(trashAction, &QAction::triggered, this,
                [this] { session_.moveLetter(letter_["hash"].toString(), "Trash"); });
        const bool trashed = letter_["folder"] == "Trash";
        const bool outgoing = letter_["folder"] == "Outbox";
        auto folderAction = [&](const char *icon, const char *text, bool visible,
                                void (Session::*act)(QString)) {
            auto action = toolbar->addAction(materialIcon(icon, iconColor(dark)), DesktopWindow::tr(text));
            action->setVisible(visible);
            connect(action, &QAction::triggered, this,
                    [this, act] { (session_.*act)(letter_["hash"].toString()); });
        };
        folderAction("restore", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Restore"), trashed, &Session::restoreLetter);
        folderAction("deleteForever", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Delete permanently"), trashed, &Session::deleteLetter);
        folderAction("retry", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Retry"), outgoing, &Session::retryLetter);
        folderAction("cancel", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Cancel delivery"), outgoing, &Session::cancelLetter);
        auto toolRow = new QHBoxLayout;
        toolRow->setContentsMargins(0, 0, 0, 0);
        toolRow->addWidget(toolbar);
        toolRow->addStretch();
        frame->addLayout(toolRow);
        auto subject = subjectArea(frame, "windowSubject");

        // Each side as "Name BM-…" when it has a local name; the address stays.
        const auto party = [this](const QString &address) {
            const auto name = session_.nameFor(address);
            return name.isEmpty() ? address : name + "  " + address;
        };
        auto addresses = new QLabel(party(letter_["from"].toString()) + "  →  " +
                                    party(letter_["to"].toString()));
        addresses->setObjectName("windowAddresses");
        addresses->setFont(addressFont());
        addresses->setTextFormat(Qt::PlainText);
        addresses->setWordWrap(true);
        addresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
        frame->addWidget(addresses);
        auto body = new LetterView;
        body->setObjectName("windowBody");
        body->setDocument(new SafeDocument(body));
        new AddressHighlighter(body->document());
        body->setOpenLinks(false);
        body->setFrameShape(QFrame::NoFrame);
        body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        connect(body, &QTextBrowser::anchorClicked, this, [this](QUrl url) {
            if (url.scheme() == "https" &&
                QMessageBox::question(this, DesktopWindow::tr("Open link"),
                                      DesktopWindow::tr("Open this link in your browser?\n%1")
                                          .arg(url.toDisplayString())) == QMessageBox::Yes)
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
    setWindowIcon(windowLogo());
    listDensity_ = QSettings().value("listDensity", "comfortable").toString();
    channelRailCollapsed_ = QSettings().value("channelRailCollapsed", false).toBool();
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
    logo->setPixmap(appLogo().pixmap(64, 64));
    logo->setFixedSize(64, 64);
    top->addWidget(logo);
    auto brand = new QLabel(
        QString("<b style='font-size:20px'>ynotbit</b><br><span "
                "style='font-size:10px'>%1</span><br><span "
                "style='font-size:9px;color:palette(mid)'>%2</span>")
            .arg(tr("PRIVATE CORRESPONDENCE").toHtmlEscaped(),
                 tr("Your keys. Your mailbox.").toHtmlEscaped()));
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
    button(tr("Close mailbox"), top, [this] { session_.closeMailbox(); })
        ->setObjectName("closeMailboxButton");
    button(tr("Lock vault"), top, [this] { session_.lock(); })->setObjectName("lockButton");
    outer->addWidget(header);
    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setContentsMargins(20, 8, 20, 8);
    outer->addWidget(error_);
    // A newer ynotbit announced by its signed release broadcast.
    updateBanner_ = new QWidget;
    updateBanner_->setObjectName("updateBanner");
    updateBanner_->setAttribute(Qt::WA_StyledBackground);
    auto updateRow = new QHBoxLayout(updateBanner_);
    updateRow->setContentsMargins(20, 6, 20, 6);
    updateLabel_ = new QLabel;
    updateLabel_->setObjectName("updateLabel");
    updateRow->addWidget(updateLabel_, 1);
    button(tr("Download"), updateRow, [this] {
        QDesktopServices::openUrl(QUrl(updates::releaseUrl(session_.availableUpdate())));
    })->setObjectName("updateDownloadButton");
    button(tr("Dismiss"), updateRow, [this] { session_.dismissUpdate(); })
        ->setObjectName("updateDismissButton");
    updateBanner_->hide();
    outer->addWidget(updateBanner_);
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
    write->setIcon(materialIcon("compose", iconColor(appearance_.dark())));
    write->setCursor(Qt::PointingHandCursor);
    connect(write, &QPushButton::clicked, this, [this] {
        if (folders_->currentRow() == 4 && !activeChannelAddress_.isEmpty())
            compose({{"to", activeChannelAddress_}, {"from", activeChannelAddress_}});
        else if (folders_->currentRow() == 5)
            compose({{"kind", "broadcast"}}); // the Broadcasts page writes one
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
                        "Trash", "Identities", "Contacts"});
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
        icon->setToolTip(folderTitle(label));
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
    auto railHead = new QLabel(tr("CHANNELS"));
    railHead->setObjectName("channelRailHeading");
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
    button(tr("+ Join or create…"), railOuter, [this] {
        if (folders_->currentRow() == 5)
            session_.subscribe();
        else
            session_.joinChannel();
        refreshChannels();
    })->setObjectName("joinOrCreateChannelButton");
    button({}, railOuter, [this] { setChannelRailCollapsed(!channelRailCollapsed_); })
        ->setObjectName("channelRailToggle");
    split->addWidget(rail);
    setChannelRailCollapsed(channelRailCollapsed_);
    auto middle = listColumn_ = new QWidget;
    middle->setObjectName("listColumn");
    middle->setFixedWidth(300);
    auto mid = new QVBoxLayout(middle);
    mid->setContentsMargins(3, 5, 3, 0);
    mid->setSpacing(2);
    heading_ = new QLabel(tr("Inbox").toUpper());
    heading_->setObjectName("listHeading");
    heading_->setStyleSheet("font-size:11px;font-weight:700;color:palette(mid);");
    mid->addWidget(heading_);
    auto search = search_ = new QLineEdit;
    search->setObjectName("messageSearch");
    search->setPlaceholderText(tr("Search this folder"));
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
        {"unread", "filterUnread", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Unread only")},
        {"anonymous", "filterAnonymous", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Anonymous only")},
    };
    for (const auto &f : filters) {
        auto btn = new QToolButton;
        btn->setObjectName(QString("filter_") + f.id);
        btn->setCheckable(true);
        btn->setFixedSize(26, 26);
        btn->setIconSize(QSize(16, 16));
        btn->setIcon(materialIcon(f.icon, iconColor(appearance_.dark())));
        btn->setToolTip(tr(f.label));
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
        {"comfortable", "densityComfortable", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Comfortable")},
        {"cozy", "densityCozy", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Cozy")},
        {"compact", "densityCompact", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Compact")},
    };
    for (const auto &d : densities) {
        auto btn = new QToolButton;
        btn->setObjectName(QString("density_") + d.id);
        btn->setCheckable(true);
        btn->setChecked(listDensity_ == d.id);
        btn->setFixedSize(26, 26);
        btn->setIconSize(QSize(16, 16));
        btn->setIcon(materialIcon(d.icon, iconColor(appearance_.dark())));
        btn->setToolTip(tr(d.label));
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
        toolbar->addAction(materialIcon("edit", iconColor(appearance_.dark())), tr("Edit / Send"));
    editAction->setObjectName("editAction");
    connect(editAction, &QAction::triggered, this, [this] { compose(selected_); });
    auto replyAction = toolbar->addAction(materialIcon("reply", iconColor(appearance_.dark())), tr("Reply"));
    replyAction->setObjectName("replyAction");
    connect(replyAction, &QAction::triggered, this, [this] { compose(selected_, true); });
    auto archiveAction =
        toolbar->addAction(materialIcon("archive", iconColor(appearance_.dark())), tr("Archive"));
    archiveAction->setObjectName("archiveAction");
    connect(archiveAction, &QAction::triggered, this,
            [this] { session_.moveLetter(selected_["hash"].toString(), "Archive"); });
    auto trashAction = toolbar->addAction(materialIcon("delete", iconColor(appearance_.dark())), tr("Trash"));
    trashAction->setObjectName("trashAction");
    connect(trashAction, &QAction::triggered, this,
            [this] { session_.moveLetter(selected_["hash"].toString(), "Trash"); });
    // Trash- and Outbox-only actions; selectMessage shows the ones
    // that apply to the selected letter.
    const struct { const char *name, *icon, *text; void (Session::*act)(QString); }
        folderActions[] = {
            {"restoreAction", "restore", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Restore"), &Session::restoreLetter},
            {"deletePermanentlyAction", "deleteForever", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Delete permanently"), &Session::deleteLetter},
            {"retryAction", "retry", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Retry"), &Session::retryLetter},
            {"cancelDeliveryAction", "cancel", QT_TRANSLATE_NOOP("bm::DesktopWindow", "Cancel delivery"), &Session::cancelLetter},
        };
    for (const auto &f : folderActions) {
        auto action = toolbar->addAction(materialIcon(f.icon, iconColor(appearance_.dark())), tr(f.text));
        action->setObjectName(f.name);
        action->setVisible(false);
        connect(action, &QAction::triggered, this,
                [this, act = f.act] { (session_.*act)(selected_["hash"].toString()); });
    }
    auto openWindowAction = toolbar->addAction(
        materialIcon("openWindow", iconColor(appearance_.dark())), tr("Open in new window"));
    openWindowAction->setObjectName("openWindowAction");
    connect(openWindowAction, &QAction::triggered, this, [this] {
        (new MessageWindow(session_, selected_, appearance_.dark(), this))->show();
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
    subject_ = subjectArea(frame, "subject");
    setSubject(subject_, tr("No letter selected"));
    details_ = new QWidget;
    auto metadata = new QGridLayout(details_);
    metadata->setContentsMargins(0, 0, 0, 0);
    metadata->setHorizontalSpacing(14);
    metadata->setVerticalSpacing(6);
    metadata->setColumnStretch(1, 1);
    // From / To: the correspondent's local name (when there is one), then the
    // address itself -- always shown, so a name can never stand in for a
    // different sender -- then a button to save an unknown address.
    fromAddress_ = new QLabel;
    toAddress_ = new QLabel;
    fromAddress_->setObjectName("messageAddresses");
    toAddress_->setObjectName("toAddress");
    fromName_ = new QLabel;
    toName_ = new QLabel;
    fromName_->setObjectName("fromName");
    toName_->setObjectName("toName");
    metadata->setColumnStretch(1, 0);
    metadata->setColumnStretch(2, 1);
    int addressRow = 0;
    for (auto [name, field] : {std::pair{fromName_, fromAddress_}, {toName_, toAddress_}}) {
        field->setFont(addressFont());
        field->setTextFormat(Qt::PlainText);
        field->setWordWrap(true);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        name->setTextFormat(Qt::PlainText);
        name->setStyleSheet("font-weight:600;");
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        name->hide();
        auto add = new QToolButton;
        add->setObjectName(addressRow == 0 ? "addSenderContact" : "addRecipientContact");
        add->setIcon(materialIcon("personAdd", iconColor(appearance_.dark())));
        add->setIconSize(QSize(16, 16));
        add->setAutoRaise(true);
        add->setCursor(Qt::PointingHandCursor);
        add->setToolTip(tr("Add to contacts"));
        add->hide();
        connect(add, &QToolButton::clicked, this, [this, field] {
            if (editContact(field->text()))
                updateCorrespondents();
        });
        (addressRow == 0 ? addFromContact_ : addToContact_) = add;
        auto label = new QLabel(addressRow == 0 ? tr("From") : tr("To"));
        if (addressRow == 1) {
            label->setObjectName("toLabel");
            toLabel_ = label;
        }
        metadata->addWidget(label, addressRow, 0, Qt::AlignTop);
        metadata->addWidget(name, addressRow, 1, Qt::AlignTop);
        auto addressCell = new QHBoxLayout;
        addressCell->setContentsMargins(0, 0, 0, 0);
        addressCell->setSpacing(4);
        addressCell->addWidget(field);
        addressCell->addWidget(add, 0, Qt::AlignTop);
        addressCell->addStretch();
        metadata->addLayout(addressCell, addressRow++, 2);
    }
    deliveryStatus_ = new QLabel;
    deliveryStatus_->setObjectName("deliveryStatus");
    deliveryStatus_->setTextFormat(Qt::PlainText);
    metadata->addWidget(deliveryStatus_, 2, 1, 1, 2, Qt::AlignLeft);
    deliveryError_ = new QLabel;
    deliveryError_->setTextFormat(Qt::PlainText);
    deliveryError_->setWordWrap(true);
    metadata->addWidget(deliveryError_, 3, 1, 1, 2);
    timeline_ = new QLabel;
    timeline_->setObjectName("messageTimeline");
    timeline_->setTextFormat(Qt::RichText);
    timeline_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timeline_->setToolTip(tr("Times are local. Received in mailbox is when the object was decrypted "
                          "and saved, which may be after network arrival while locked. Sent to "
                          "peers is a relay offer, not a read receipt."));
    metadata->addWidget(timeline_, 4, 0, 1, 3);
    details_->hide();
    frame->addWidget(details_);
    body_ = new LetterView;
    body_->setObjectName("readerBody");
    body_->setDocument(new SafeDocument(body_));
    new AddressHighlighter(body_->document());
    body_->setOpenLinks(false);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    frame->addWidget(body_, 1);
    connect(body_, &QTextBrowser::anchorClicked, this, [this](QUrl url) {
        if (url.scheme() == "https" &&
            QMessageBox::question(this, tr("Open link"),
                                  tr("Open this link in your browser?\n%1")
                                      .arg(url.toDisplayString())) == QMessageBox::Yes)
            QDesktopServices::openUrl(url);
    });
    welcomeStack_ = new QStackedWidget;
    welcomeStack_->setObjectName("welcomeStack");
    // State 1: vault locked (or none chosen yet). A vault target is shown with an
    // inline password field instead of a modal dialog; recents let you switch targets.
    lockedPage_ = new QWidget;
    // Scrolls rather than squeezes when the window is shorter than the card
    // (Windows lets it get that short): squeezed, every box clips its text.
    auto lockedScroll = new QScrollArea;
    lockedScroll->setObjectName("lockedScroll");
    lockedScroll->setWidgetResizable(true);
    lockedScroll->setFrameShape(QFrame::NoFrame);
    lockedScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lockedScroll->viewport()->setAutoFillBackground(false);
    auto lockedPageLayout = new QVBoxLayout(lockedPage_);
    lockedPageLayout->setContentsMargins(0, 0, 0, 0);
    lockedPageLayout->addWidget(lockedScroll);
    auto lockedContent = new QWidget;
    lockedContent->setObjectName("lockedContent");
    lockedContent->setAutoFillBackground(false);
    lockedScroll->setWidget(lockedContent);
    auto lockedOuter = new QVBoxLayout(lockedContent);
    lockedOuter->addStretch();
    auto lockedCard = new FixedWidthColumn;
    lockedCard->setFixedWidth(380);
    auto lockedCardLayout = new QVBoxLayout(lockedCard);
    lockedCardLayout->setSpacing(14);
    auto lockedTitle =
        new QLabel("<b style='font-size:16px'>" + tr("Vault locked").toHtmlEscaped() + "</b>");
    lockedCardLayout->addWidget(lockedTitle);
    auto lockedSubtitle = new QLabel(tr("Choose a vault file and enter its passphrase to unlock."));
    lockedSubtitle->setObjectName("lockedSubtitle");
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
        "QWidget#vaultBox{border:1px solid palette(mid);border-radius:8px;}");
    // Spacing from the layout, not stylesheet padding: on macOS that padding
    // shrinks the inside without growing the box, clipping the name and path.
    vaultBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto vaultBoxLayout = new QVBoxLayout(vaultBox);
    vaultBoxLayout->setContentsMargins(12, 10, 12, 10);
    vaultBoxLayout->setSpacing(2);
    lockedVaultName_ = new QLabel;
    lockedVaultName_->setObjectName("lockedVaultName");
    lockedVaultName_->setStyleSheet("font-weight:700;");
    lockedVaultPath_ = new ElidedLabel;
    lockedVaultPath_->setObjectName("lockedVaultPath");
    lockedVaultPath_->setFont(addressFont());
    // However long the path, it doesn't widen the card.
    lockedVaultPath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    lockedVaultPath_->setStyleSheet("color:palette(mid);font-size:11px;");
    vaultBoxLayout->addWidget(lockedVaultName_);
    vaultBoxLayout->addWidget(lockedVaultPath_);
    authLayout->addWidget(vaultBox);
    vaultPasswordField_ = new QLineEdit;
    vaultPasswordField_->setObjectName("vaultPasswordField");
    vaultPasswordField_->setEchoMode(QLineEdit::Password);
    vaultPasswordField_->setPlaceholderText(tr("Passphrase"));
    authLayout->addWidget(vaultPasswordField_);
    vaultRepeatField_ = new QLineEdit;
    vaultRepeatField_->setObjectName("vaultRepeatField");
    vaultRepeatField_->setEchoMode(QLineEdit::Password);
    vaultRepeatField_->setPlaceholderText(tr("Repeat passphrase"));
    authLayout->addWidget(vaultRepeatField_);
    vaultUnlockButton_ = new QPushButton(tr("Unlock vault"));
    vaultUnlockButton_->setObjectName("vaultUnlockButton");
    authLayout->addWidget(vaultUnlockButton_);
    lockedCardLayout->addWidget(authGroup);
    auto lockedRecentsLabel = new QLabel(tr("RECENT VAULTS"));
    lockedRecentsLabel->setStyleSheet("color:palette(mid);font-size:11px;font-weight:700;");
    lockedCardLayout->addWidget(lockedRecentsLabel);
    auto lockedRecentsContainer = new QWidget;
    lockedRecentsLayout_ = new QVBoxLayout(lockedRecentsContainer);
    lockedRecentsLayout_->setContentsMargins(0, 0, 0, 0);
    lockedCardLayout->addWidget(lockedRecentsContainer);
    button(tr("Open vault file from disk…"), lockedCardLayout,
           [this] { session_.beginVaultOpen(); })
        ->setObjectName("openVaultButton");
    button(tr("Create a new vault…"), lockedCardLayout, [this] { session_.beginVaultCreate(); })
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
    auto noMailTitle =
        new QLabel("<b style='font-size:16px'>" + tr("No mailbox open").toHtmlEscaped() + "</b>");
    noMailLayout->addWidget(noMailTitle);
    auto noMailSubtitle =
        new QLabel(tr("Your vault is unlocked. Choose a mailbox to open, or open one from disk."));
    noMailSubtitle->setStyleSheet("color:palette(mid);");
    noMailSubtitle->setWordWrap(true);
    noMailLayout->addWidget(noMailSubtitle);
    auto noMailRecentsLabel = new QLabel(tr("RECENT MAILBOXES"));
    noMailRecentsLabel->setStyleSheet("color:palette(mid);font-size:11px;font-weight:700;");
    noMailLayout->addWidget(noMailRecentsLabel);
    auto noMailRecentsContainer = new QWidget;
    noMailboxRecentsLayout_ = new QVBoxLayout(noMailRecentsContainer);
    noMailboxRecentsLayout_->setContentsMargins(0, 0, 0, 0);
    noMailLayout->addWidget(noMailRecentsContainer);
    button(tr("Open mailbox file from disk…"), noMailLayout, [this] { session_.openMailbox(); })
        ->setObjectName("openMailboxButton");
    button(tr("Create a new mailbox…"), noMailLayout, [this] { session_.createMailbox(); })
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
    auto identitiesHeading = new QLabel(tr("Identities & chans"));
    identitiesHeading->setObjectName("identitiesHeading");
    identitiesHeading->setStyleSheet("font-size:20px;font-weight:600;");
    identitiesTitleCol->addWidget(identitiesHeading);
    auto identitiesSub = new QLabel(tr("Addresses you can send mail from"));
    identitiesSub->setStyleSheet("color:palette(mid);font-size:12px;");
    identitiesTitleCol->addWidget(identitiesSub);
    identitiesHeadRow->addLayout(identitiesTitleCol);
    identitiesHeadRow->addStretch();
    button(tr("Join a channel…"), identitiesHeadRow, [this] {
        session_.joinChannel();
        refreshIdentities();
    })->setObjectName("joinChannelButton");
    button(tr("New identity"), identitiesHeadRow, [this] {
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
    // Subscriptions: a feed of the selected sender's posts instead of the list
    // and the reader.
    feed_ = new FeedView(session_);
    feed_->hide();
    read->addWidget(feed_, 1);
    connect(feed_, &FeedView::replyRequested, this,
            [this](const QVariantMap &letter) { compose(letter, true); });
    connect(feed_, &FeedView::openRequested, this, [this](const QVariantMap &letter) {
        (new MessageWindow(session_, letter, appearance_.dark(), this))->show();
    });
    connect(feed_, &FeedView::addContactRequested, this, [this](const QString &address) {
        if (editContact(address))
            feed_->setSource(feed_->source(), feed_->label()); // names on every card
    });
    // A post read in the feed may clear its sender's bold in the rail.
    connect(&session_, &Session::messageRead, this, [this] {
        if (folders_->currentRow() == 5)
            QTimer::singleShot(0, this, [this] { refreshChannels(); });
    });
    // The address book: same card layout as identities, for other people.
    contacts_ = new QWidget;
    contacts_->setObjectName("contactsPane");
    auto contactsOuter = new QVBoxLayout(contacts_);
    contactsOuter->setContentsMargins(4, 4, 4, 0);
    contactsOuter->setSpacing(8);
    auto contactsHeadRow = new QHBoxLayout;
    auto contactsTitleCol = new QVBoxLayout;
    auto contactsHeading = new QLabel(tr("Contacts"));
    contactsHeading->setObjectName("contactsHeading");
    contactsHeading->setStyleSheet("font-size:20px;font-weight:600;");
    contactsTitleCol->addWidget(contactsHeading);
    auto contactsSub = new QLabel(tr("People you write to. Names are private to this mailbox."));
    contactsSub->setStyleSheet("color:palette(mid);font-size:12px;");
    contactsTitleCol->addWidget(contactsSub);
    contactsHeadRow->addLayout(contactsTitleCol);
    contactsHeadRow->addStretch();
    button(tr("Add contact…"), contactsHeadRow, [this] {
        if (editContact({}))
            refreshContacts();
    })->setObjectName("addContactButton");
    contactsOuter->addLayout(contactsHeadRow);
    contactsFilter_ = new QLineEdit;
    contactsFilter_->setObjectName("contactsFilter");
    contactsFilter_->setPlaceholderText(tr("Filter by name or address"));
    contactsFilter_->setClearButtonEnabled(true);
    connect(contactsFilter_, &QLineEdit::textChanged, this, [this] { refreshContacts(); });
    contactsOuter->addWidget(contactsFilter_);
    auto contactsScroll = new QScrollArea;
    contactsScroll->setObjectName("contactsScroll");
    contactsScroll->setWidgetResizable(true);
    contactsScroll->setFrameShape(QFrame::NoFrame);
    contactsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto contactsListContainer = new QWidget;
    contactLayout_ = new QVBoxLayout(contactsListContainer);
    contactLayout_->setContentsMargins(0, 4, 4, 16);
    contactLayout_->setSpacing(10);
    contactsScroll->setWidget(contactsListContainer);
    contactsOuter->addWidget(contactsScroll, 1);
    read->addWidget(contacts_);
    split->addWidget(reader_, 1);
    status_ = new QLabel;
    status_->setObjectName("statusBar");
    status_->setContentsMargins(24, 12, 24, 12);
    outer->addWidget(status_);
    auto file = menuBar()->addMenu(tr("File"));
    file->addAction(tr("Create vault…"), &session_, &Session::beginVaultCreate);
    file->addAction(tr("Open vault…"), &session_, &Session::beginVaultOpen);
    file->addAction(tr("Create mailbox…"), &session_, &Session::createMailbox);
    file->addAction(tr("Open mailbox…"), &session_, &Session::openMailbox);
    file->addAction(tr("Close mailbox"), &session_, &Session::closeMailbox);
    file->addAction(tr("Back up mailbox and vault…"), &session_, &Session::backup);
    file->addAction(tr("Lock vault"), QKeySequence("Ctrl+L"), &session_, &Session::lock);
    auto network = menuBar()->addMenu(tr("Network"));
    auto enabled = network->addAction(tr("Network enabled"));
    enabled->setCheckable(true);
    enabled->setChecked(session_.networkEnabled());
    connect(enabled, &QAction::toggled, &session_, &Session::setNetworkEnabled);
    network->addAction(tr("Peer / proxy settings…"), &session_, &Session::configureNode);
    network->addAction(tr("Restart node"), &session_, &Session::restartNode);
    network->addAction(tr("Retention settings…"), &session_, &Session::configureRetention);
    network->addSeparator();
    updateNoticesAction_ = network->addAction(tr("Notify about new ynotbit versions"));
    updateNoticesAction_->setObjectName("updateNoticesAction");
    updateNoticesAction_->setCheckable(true);
    connect(updateNoticesAction_, &QAction::triggered, &session_, &Session::setUpdateNotices);
    auto identity = menuBar()->addMenu(tr("Identity"));
    identity->addAction(tr("Create identity…"), &session_, &Session::addIdentity);
    identity->addAction(tr("Join or create chan…"), &session_, &Session::joinChannel);
    identity->addAction(tr("Import keys.dat…"), &session_, &Session::importIdentities);
    identity->addAction(tr("Change vault password…"), &session_, &Session::changePassword);
    identity->addAction(tr("Subscribe to broadcasts…"), &session_, &Session::subscribe);
    identity->addAction(tr("Manage subscriptions…"), this, [this] {
        QStringList labels, addresses;
        for (auto v : session_.subscriptions()) {
            auto m = v.toMap();
            labels << m["label"].toString() + " · " + m["address"].toString();
            addresses << m["address"].toString();
        }
        bool ok;
        QInputDialog dialog(this);
        dialog.setWindowTitle(tr("Unsubscribe"));
        dialog.setLabelText(tr("Subscription"));
        dialog.setComboBoxItems(labels);
        dialog.setComboBoxEditable(false);
        dialog.setFont(addressFont());
        ok = dialog.exec() == QDialog::Accepted;
        auto value = dialog.textValue();
        if (ok && labels.contains(value))
            session_.unsubscribe(addresses[labels.indexOf(value)]);
    });
    identity->addAction(tr("Inspect retained objects again"), &session_, &Session::rescan);
    auto appearance = menuBar()->addMenu(tr("Appearance"));
    auto group = new QActionGroup(this);
    const std::pair<QString, QString> modes[] = {
        {"system", tr("System")}, {"light", tr("Light")}, {"dark", tr("Dark")}};
    for (const auto &[mode, label] : modes) {
        auto a = appearance->addAction(label);
        a->setObjectName("appearance_" + mode);
        a->setCheckable(true);
        a->setChecked(appearance_.mode() == mode);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode] { appearance_.setMode(mode); });
    }
    // Language, under Appearance: applied at startup, so a change takes effect
    // on restart.
    appearance->addSeparator();
    auto languageMenu = appearance->addMenu(tr("Language"));
    languageMenu->setObjectName("languageMenu");
    auto languageGroup = new QActionGroup(this);
    auto addLanguage = [&](const QString &code, const QString &label) {
        auto a = languageMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(savedLanguage() == code);
        languageGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, code] {
            if (code == savedLanguage())
                return;
            saveLanguage(code);
            QMessageBox::information(this, tr("Language"),
                                     tr("Restart ynotbit to use the new language."));
        });
    };
    addLanguage({}, tr("System default"));
    languageMenu->addSeparator();
    for (const auto &language : languages())
        addLanguage(language.code, language.nativeName);
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
        setSubject(subject_, tr("No letter selected"));
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
                    setSubject(subject_, tr("No letter selected"));
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
        setSubject(subject_, tr("No letter selected"));
        clearDetails();
        actions_->hide();
    });
    connect(folders_, &QListWidget::currentTextChanged, this, [this](QString folder) {
        if (auto icon = findChild<QToolButton *>("folderIcon_" + folder))
            icon->setChecked(true);
        // Folder names are keys; the heading shows the translated name.
        heading_->setText(folderTitle(folder).toUpper());
        if (folder == "Channels" || folder == "Broadcasts")
            refreshChannels();
        session_.messageModel()->setFolder(folder);
        selected_.clear();
        body_->clear();
        setSubject(subject_, folder == "Identities" ? tr("Identities & chans")
                             : folder == "Contacts" ? "Contacts"
                                                    : tr("No letter selected"));
        clearDetails();
        actions_->hide();
        if (folder == "Identities")
            refreshIdentities();
        if (folder == "Contacts")
            refreshContacts();
        updateState();
    });
    folders_->setCurrentRow(0);
    updateTheme();
    updateState();
}
void DesktopWindow::updateTheme() {
    bool dark = appearance_.dark();
    if (feed_)
        feed_->setDark(dark);
    setStyleSheet(
        QString(
            "QWidget{font-size:13px;} QMainWindow,QDialog{background:%1;} "
            "QWidget#sidebar{background:%2;} QWidget#header{background:%3;} "
            "QWidget#statusBar{background:%3;font-size:11px;} "
            "QPushButton,QLineEdit,QComboBox{padding:9px;border:1px solid %4;border-radius:6px;} "
            "QPushButton:hover{background:%2;} QListView,QTextEdit{border:0;} "
            "QListWidget::item{padding:10px;} QListWidget::item:selected{background:%5;color:%6;} "
            "QToolBar{border:0;spacing:3px;} "
            "QPushButton:default{background:%6;color:%1;border-color:%6;font-weight:600;} "
            "QPushButton:default:hover{background:%6;border-color:%4;} "
            "QPushButton#contactsPickerButton::menu-indicator{image:none;width:0;} "
            "QWidget#updateBanner{background:%5;} QLabel#updateLabel{color:%6;font-weight:600;} "
            // No bold on :checked -- it would widen the label past its size hint.
            "QWidget#modeSwitch{border:1px solid %4;border-radius:7px;background:transparent;} "
            "QWidget#modeSwitch QPushButton{border:0;border-radius:0;background:transparent;"
            "padding:7px 14px;} "
            "QWidget#modeSwitch QPushButton:hover{background:%3;} "
            "QWidget#modeSwitch QPushButton:checked{background:%5;color:%6;} "
            "QWidget#modeSwitch QPushButton#modePrivateButton{border-top-left-radius:6px;"
            "border-bottom-left-radius:6px;border-right:1px solid %4;} "
            "QWidget#modeSwitch QPushButton#modePublicButton{border-top-right-radius:6px;"
            "border-bottom-right-radius:6px;} "
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
            "border-top-left-radius:6px;border-bottom-left-radius:6px;"
            "border-right:1px solid %4;} "
            "QWidget[viewSwitch=\"true\"] QToolButton#view_text{border-right:1px solid %4;} "
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
    findChild<QAction *>("openWindowAction")->setIcon(materialIcon("openWindow", color));
    findChild<QAction *>("restoreAction")->setIcon(materialIcon("restore", color));
    findChild<QAction *>("deletePermanentlyAction")->setIcon(materialIcon("deleteForever", color));
    findChild<QAction *>("retryAction")->setIcon(materialIcon("retry", color));
    findChild<QAction *>("cancelDeliveryAction")->setIcon(materialIcon("cancel", color));
    findChild<QPushButton *>("writeButton")->setIcon(materialIcon("compose", color));
    static_cast<ViewSwitch *>(viewSwitch_)->setIconColor(color);
    for (const auto &[id, icon] : {std::pair{"density_comfortable", "densityComfortable"},
                                   {"density_cozy", "densityCozy"},
                                   {"density_compact", "densityCompact"},
                                   {"filter_unread", "filterUnread"},
                                   {"filter_anonymous", "filterAnonymous"}})
        findChild<QToolButton *>(id)->setIcon(materialIcon(icon, color));
    for (const auto &[label, iconName] : kFolderIcons)
        findChild<QToolButton *>("folderIcon_" + label)->setIcon(materialIcon(iconName, color));
    for (auto add : {addFromContact_, addToContact_})
        static_cast<QToolButton *>(add)->setIcon(materialIcon("personAdd", color));
    if (contacts_->isVisible())
        refreshContacts();
}
void DesktopWindow::clearDetails() {
    fromAddress_->clear();
    toAddress_->clear();
    updateCorrespondents();
    deliveryStatus_->clear();
    deliveryError_->clear();
    timeline_->clear();
    details_->hide();
    static_cast<KindFrame *>(letterStripe_)->setKind(LetterKind::Personal);
}
void DesktopWindow::updateDeliveryStatus() {
    const auto state = selected_.value("state").toString();
    const bool success = state == "acknowledged";
    const auto label = deliveryStateLabel(state);
    deliveryStatus_->setText(label);
    deliveryStatus_->setVisible(!label.isEmpty());
    deliveryStatus_->setToolTip(
        success ? tr("Recipient acknowledged delivery. This is not a read receipt.") : QString());
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
        const auto label = folder == "Drafts" ? tr("Draft saved")
                                              : (folder == "Inbox" || folder == "Channels" ||
                                                         folder == "Broadcasts"
                                                     ? tr("Received in mailbox")
                                                     : tr("Saved in mailbox"));
        row(label, formatDateTime(
                       QDateTime::fromSecsSinceEpoch(selected_.value("storedAt").toLongLong()),
                       true));
    } else {
        const QMap<QString, QString> labels{{"queued", tr("Queued")},
                                            {"awaiting_pubkey", tr("Requested recipient key")},
                                            {"key_available", tr("Recipient key available")},
                                            {"calculating_ack", tr("Receipt preparation started")},
                                            {"calculating_message", tr("Message preparation started")},
                                            {"prepared", tr("Prepared")},
                                            {"publishing", tr("Submitted to relay")},
                                            {"published", tr("Sent to peers")},
                                            {"awaiting_ack", tr("Sent to peers")},
                                            {"acknowledged", tr("Acknowledged")},
                                            {"failed", tr("Failed")},
                                            {"expired", tr("Expired")},
                                            {"cancelled", tr("Cancelled")}};
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
            row(deliveryStateLabel(current), formatDateTime(QDateTime::currentDateTime(), true));
        }
    }
    timeline_->setText(html + "</table>");
    updateDeliveryStatus();
}
void DesktopWindow::updateState() {
    // A mailbox opens on Subscriptions with the digest selected (the first
    // sender instead, if the digest was unsubscribed).
    if (session_.mailboxOpen())
        if (const auto welcome = session_.takeWelcomeSource(); !welcome.isEmpty()) {
            activeBroadcastAddress_ = welcome;
            if (folders_->currentRow() == 5)
                refreshChannels();
            else
                folders_->setCurrentRow(5); // runs updateState again
            return;
        }
    setWindowTitle(session_.document() + " — ynotbit");
    error_->setText(session_.error());
    error_->setVisible(!session_.error().isEmpty());
    const auto update = session_.availableUpdate();
    updateLabel_->setText(tr("ynotbit %1 is available (you have %2).")
                              .arg(update, QCoreApplication::applicationVersion()));
    updateBanner_->setVisible(!update.isEmpty());
    updateNoticesAction_->setEnabled(session_.mailboxOpen() &&
                                     !updates::publisherAddress().isEmpty());
    updateNoticesAction_->setChecked(session_.updateNotices());
    findChild<QPushButton *>("lockButton")->setVisible(session_.unlocked());
    findChild<QPushButton *>("closeMailboxButton")->setVisible(session_.mailboxOpen());
    const bool channelPage = folders_->currentRow() == 4;
    const bool broadcastPage = folders_->currentRow() == 5;
    const bool mailboxState = session_.mailboxOpen();
    auto write = findChild<QPushButton *>("writeButton");
    write->setToolTip(channelPage     ? tr("Write to channel")
                      : broadcastPage ? tr("Write a broadcast")
                                      : tr("Write a letter"));
    write->setEnabled(mailboxState && (!channelPage || !activeChannelAddress_.isEmpty()));
    channelRail_->setVisible(mailboxState && (channelPage || broadcastPage));
    heading_->setVisible(!channelPage && !broadcastPage);
    channelRail_->setEnabled(session_.unlocked());
    updateListCount();
    const auto identities = session_.unlocked() ? session_.identities() : QVariantList();
    if (identities != channelIdentities_) {
        channelIdentities_ = identities;
        refreshChannels();
        // Created, joined or imported from the menu while the page is open. Later,
        // not now: a card's own button may be what changed them, mid-click.
        if (folders_->currentRow() == 8)
            QTimer::singleShot(0, this, [this] {
                if (folders_->currentRow() == 8)
                    refreshIdentities();
            });
    }
    // After refreshChannels(), which may have picked a different active chan.
    // Whole sentences, so languages can inflect "folder"/"channel" as they need.
    QString placeholder = tr("Search this folder");
    if (channelPage) {
        placeholder = tr("Search this channel");
        for (auto v : session_.channels())
            if (v.toMap()["address"].toString() == activeChannelAddress_)
                placeholder = tr("Search %1").arg(v.toMap()["label"].toString());
    }
    // Subscriptions or update notices changed: the Broadcasts rail follows.
    if (const auto sources = session_.broadcastSources(); sources != shownBroadcastSources_) {
        shownBroadcastSources_ = sources;
        if (broadcastPage)
            QTimer::singleShot(0, this, [this] { refreshChannels(); });
    }
    if (broadcastPage) {
        placeholder = tr("Search these broadcasts");
        for (auto v : shownBroadcastSources_)
            if (v.toMap()["address"].toString() == activeBroadcastAddress_)
                placeholder = tr("Search %1").arg(v.toMap()["label"].toString());
    }
    search_->setPlaceholderText(placeholder);
    // A contact or subscription added, renamed or removed anywhere redraws every
    // view of names.
    if (const auto subscriptions = session_.subscriptions(); subscriptions != shownSubscriptions_) {
        shownSubscriptions_ = subscriptions;
        updateCorrespondents();
    }
    if (const auto contacts = session_.contacts(); contacts != shownContacts_) {
        shownContacts_ = contacts;
        updateCorrespondents();
        if (folders_->currentRow() == 9)
            refreshContacts();
    }
    // Identities and Contacts are whole-pane pages: no letter list, no reader.
    const bool contactsPage = folders_->currentRow() == 9 && mailboxState;
    const bool identityPage = (folders_->currentRow() == 8 && mailboxState) || contactsPage;
    sidebarWidget_->setVisible(mailboxState);
    // Subscriptions is a feed: no letter list, no reader.
    const bool feedPage = folders_->currentRow() == 5 && mailboxState;
    listColumn_->setVisible(mailboxState && !identityPage && !feedPage);
    feed_->setVisible(feedPage);
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
    subject_->setVisible(mailboxState && !identityPage && !feedPage);
    body_->setVisible(mailboxState && !identityPage && !feedPage);
    letterStripe_->setVisible(mailboxState && !identityPage && !feedPage);
    identities_->setVisible(identityPage && !contactsPage);
    contacts_->setVisible(contactsPage);
    status_->setText(
        !session_.unlocked()
            ? tr("Vault locked") +
                  (targetVaultPath_.isEmpty()
                       ? QString()
                       : " · " + QFileInfo(targetVaultPath_).fileName()) +
                  " · " + tr("keys not loaded")
        : !mailboxState
            ? tr("Vault unlocked · no mailbox loaded")
            : session_.status() + " · " +
                  tr("cached objects: %1 · %2 MB")
                      .arg(session_.objectCount())
                      .arg(session_.cacheBytes() / 1048576.0, 0, 'f', 1) +
                  "\n" + session_.activity());
}
void DesktopWindow::refreshChannels() {
    // One rail, two pages: chans on Channels, subscriptions on Broadcasts.
    const bool broadcasts = folders_->currentRow() == 5;
    updateRailTexts();
    const auto entries = broadcasts ? session_.broadcastSources() : session_.channels();
    auto &active = broadcasts ? activeBroadcastAddress_ : activeChannelAddress_;
    bool stillJoined = false;
    for (auto v : entries)
        stillJoined = stillJoined || v.toMap()["address"].toString() == active;
    if (!stillJoined)
        active = entries.isEmpty() ? QString() : entries.first().toMap()["address"].toString();
    while (auto item = channelChipLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (auto v : entries) {
        auto item = v.toMap();
        auto chipAddress = item["address"].toString();
        auto label = item["label"].toString();
        // Collapsed, the rail is a column of identicons; the name moves to
        // the tooltip.
        auto chip = new QPushButton(channelRailCollapsed_ ? QString() : label);
        chip->setObjectName("channelChip");
        chip->setCheckable(true);
        chip->setChecked(chipAddress == active);
        const bool unread = broadcasts ? session_.broadcastUnread(chipAddress)
                                       : session_.channelUnread(chipAddress);
        if (channelRailCollapsed_) {
            // No name to embolden when collapsed: unread mail is a dot on the
            // identicon's corner instead.
            QPixmap badge(24, 24);
            badge.fill(Qt::transparent);
            QPainter p(&badge);
            p.drawPixmap(3, 3, identiconPixmap(chipAddress, 18));
            if (unread) {
                p.setRenderHint(QPainter::Antialiasing);
                // On the selected chip the background is the highlight colour
                // itself, so the dot takes the text colour drawn on it.
                const bool selected = chipAddress == active;
                p.setPen(QPen(palette().color(selected ? QPalette::Highlight : QPalette::Window),
                              1.5));
                p.setBrush(palette().color(selected ? QPalette::HighlightedText
                                                    : QPalette::Highlight));
                p.drawEllipse(QRectF(16.5, 0.75, 7, 7));
            }
            p.end();
            chip->setIcon(QIcon(badge));
            chip->setIconSize(QSize(24, 24));
        } else {
            chip->setIcon(QIcon(identiconPixmap(chipAddress, 18)));
            chip->setIconSize(QSize(18, 18));
        }
        if (unread) {
            auto f = chip->font();
            f.setBold(true);
            chip->setFont(f);
        }
        chip->setStyleSheet(QString("QPushButton{text-align:%1;padding:%2;border:0;"
                                    "border-radius:6px;} QPushButton:checked{"
                                    "background:palette(highlight);"
                                    "color:palette(highlighted-text);}")
                                .arg(channelRailCollapsed_ ? "center" : "left",
                                     channelRailCollapsed_ ? "5px 4px" : "8px 10px"));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip((label != chipAddress ? "<b>" + label.toHtmlEscaped() + "</b>" : QString()) +
                         "<pre>" + chipAddress.toHtmlEscaped() + "</pre>");
        connect(chip, &QPushButton::clicked, this, [this, chipAddress, broadcasts] {
            (broadcasts ? activeBroadcastAddress_ : activeChannelAddress_) = chipAddress;
            session_.messageModel()->setChannel(chipAddress);
            updateState();
            QTimer::singleShot(0, this, [this] { refreshChannels(); });
        });
        if (broadcasts) {
            // Right-click: copy the address, or stop following this sender.
            const bool subscribed = item["subscribed"].toBool();
            const bool updatesSource = item["updates"].toBool();
            chip->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(chip, &QWidget::customContextMenuRequested, this,
                    [this, chip, chipAddress, subscribed, updatesSource](const QPoint &at) {
                        QMenu menu(this);
                        menu.setObjectName("broadcastSourceMenu");
                        menu.addAction(tr("Copy address"))->setObjectName("copySourceAddress");
                        if (updatesSource && session_.updateNotices())
                            menu.addAction(tr("Turn off update notices"))
                                ->setObjectName("turnOffUpdates");
                        else if (subscribed)
                            menu.addAction(tr("Unsubscribe"))->setObjectName("unsubscribeSource");
                        // Acted on once the menu is closed: these redraw the rail,
                        // and with it this chip.
                        const auto chosen = menu.exec(chip->mapToGlobal(at));
                        const auto what = chosen ? chosen->objectName() : QString();
                        if (what == "copySourceAddress")
                            session_.copyAddress(chipAddress);
                        else if (what == "turnOffUpdates")
                            session_.setUpdateNotices(false);
                        else if (what == "unsubscribeSource")
                            session_.unsubscribe(chipAddress);
                    });
        }
        channelChipLayout_->addWidget(chip);
    }
    channelChipLayout_->addStretch();
    session_.messageModel()->setChannel(active);
    if (broadcasts) {
        QString label;
        for (auto v : entries)
            if (v.toMap()["address"].toString() == active)
                label = v.toMap()["label"].toString();
        if (feed_->source() != active || feed_->label() != label)
            feed_->setSource(active, label);
        else
            feed_->refresh();
    }
    if (folders_->currentRow() == 4)
        heading_->setText(tr("Channels"));
}
void DesktopWindow::updateRailTexts() {
    const bool broadcasts = folders_->currentRow() == 5;
    auto heading = channelRail_->findChild<QLabel *>("channelRailHeading");
    heading->setText(channelRailCollapsed_ ? (broadcasts ? "@" : "#")
                                           : (broadcasts ? tr("SUBSCRIPTIONS") : tr("CHANNELS")));
    auto join = channelRail_->findChild<QPushButton *>("joinOrCreateChannelButton");
    join->setText(channelRailCollapsed_ ? "+"
                  : broadcasts          ? tr("+ Subscribe…")
                                        : tr("+ Join or create…"));
    join->setToolTip(!channelRailCollapsed_ ? QString()
                     : broadcasts           ? tr("Subscribe to broadcasts")
                                            : tr("Join or create a chan"));
}
void DesktopWindow::setChannelRailCollapsed(bool collapsed) {
    channelRailCollapsed_ = collapsed;
    QSettings().setValue("channelRailCollapsed", collapsed);
    channelRail_->setFixedWidth(collapsed ? 50 : 190);
    static_cast<QVBoxLayout *>(channelRail_->layout())
        ->setContentsMargins(collapsed ? 6 : 10, 5, collapsed ? 4 : 2, 12);
    channelRail_->findChild<QLabel *>("channelRailHeading")
        ->setAlignment(collapsed ? Qt::AlignHCenter : Qt::AlignLeft);
    auto toggle = channelRail_->findChild<QPushButton *>("channelRailToggle");
    toggle->setText(collapsed ? ">>>" : "<<<");
    toggle->setToolTip(collapsed ? tr("Expand the channel list") : tr("Collapse the channel list"));
    refreshChannels();
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
    listCountLabel_->setText(shown == total ? tr("%1 total").arg(total)
                                            : tr("%1 of %2 total").arg(shown).arg(total));
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
    // A broadcast's recipient is its own sender: no To line.
    const bool broadcast = selected_["folder"].toString() == "Broadcasts";
    toAddress_->setText(broadcast ? QString() : selected_["to"].toString());
    toAddress_->setVisible(!broadcast);
    toLabel_->setVisible(!broadcast);
    updateCorrespondents();
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
void DesktopWindow::updateCorrespondents() {
    for (auto [name, field, add] : {std::tuple{fromName_, fromAddress_, addFromContact_},
                                    {toName_, toAddress_, addToContact_}}) {
        const auto address = field->text();
        const auto known = session_.nameFor(address);
        name->setText(known);
        name->setVisible(!known.isEmpty());
        // Offered for any foreign address not yet in the book -- including a
        // subscription, whose label is not a contact.
        add->setVisible(!address.isEmpty() && session_.contactProblem(address).isEmpty() &&
                        !session_.isContact(address));
    }
}
bool DesktopWindow::editContact(QString address, QString label) {
    ContactDialog dialog(session_, address, label, this);
    return dialog.exec() == QDialog::Accepted;
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
    static_cast<ElidedLabel *>(lockedVaultPath_)->setFullText(targetVaultPath_);
    findChild<QWidget *>("vaultAuthGroup")->setVisible(hasTarget);
    vaultRepeatField_->setVisible(vaultCreateMode_);
    vaultUnlockButton_->setText(vaultCreateMode_ ? tr("Create vault") : tr("Unlock vault"));
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
            auto badge = new QLabel(tr("Default"));
            badge->setObjectName("defaultBadge");
            badge->setStyleSheet("font-size:11px;font-weight:600;padding:2px 7px;"
                                 "border-radius:5px;background:palette(highlight);"
                                 "color:palette(highlighted-text);");
            nameRow->addWidget(badge);
        }
        if (m["chan"].toBool()) {
            auto chanBadge = new QLabel(tr("Channel"));
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
        copyButton->setToolTip(tr("Copy address"));
        connect(copyButton, &QToolButton::clicked, this,
                [this, address] { session_.copyAddress(address); });
        addressRow->addWidget(copyButton);
        addressRow->addStretch();
        infoCol->addLayout(addressRow);
        cardRow->addLayout(infoCol, 1);

        if (!isDefault) {
            auto setDefaultButton = new QPushButton(tr("Set as default"));
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
        qrButton->setToolTip(tr("Show QR code"));
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
            button(tr("Copy address"), buttonsRow, [this, address] { session_.copyAddress(address); });
            auto closeButton = new QPushButton(tr("Close"));
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
        renameButton->setToolTip(tr("Rename"));
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
        deleteButton->setToolTip(tr("Delete"));
        connect(deleteButton, &QToolButton::clicked, this, [this, address] {
            session_.deleteIdentity(address);
            QTimer::singleShot(0, this, [this] { refreshIdentities(); });
        });
        cardRow->addWidget(deleteButton);

        identityLayout_->addWidget(card);
    }
    identityLayout_->addStretch();
}
void DesktopWindow::refreshContacts() {
    while (auto item = contactLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto color = iconColor(appearance_.dark());
    const auto filter = contactsFilter_->text().trimmed();
    const auto all = session_.contacts();
    int shown = 0;
    for (auto v : all) {
        auto m = v.toMap();
        const auto address = m["address"].toString();
        const auto label = m["label"].toString();
        if (!filter.isEmpty() && !label.contains(filter, Qt::CaseInsensitive) &&
            !address.contains(filter, Qt::CaseInsensitive))
            continue;
        ++shown;
        auto card = new QFrame;
        card->setObjectName("contactCard");
        card->setStyleSheet("QFrame#contactCard{border:1px solid palette(mid);border-radius:8px;}");
        auto cardRow = new QHBoxLayout(card);
        cardRow->setContentsMargins(14, 12, 14, 12);
        cardRow->setSpacing(10);
        auto identicon = new QLabel;
        identicon->setFixedSize(40, 40);
        identicon->setPixmap(identiconPixmap(address, 40));
        cardRow->addWidget(identicon);
        auto infoCol = new QVBoxLayout;
        auto nameLabel = new QLabel(label);
        nameLabel->setObjectName("contactName");
        nameLabel->setStyleSheet("font-weight:700;");
        nameLabel->setTextFormat(Qt::PlainText);
        if (label.contains("BM-"))
            nameLabel->setFont(addressFont());
        infoCol->addWidget(nameLabel);
        auto addressRow = new QHBoxLayout;
        auto addressLabel = new QLabel(address);
        addressLabel->setObjectName("contactAddress");
        addressLabel->setFont(addressFont());
        addressLabel->setStyleSheet("color:palette(mid);font-size:12px;");
        addressLabel->setTextFormat(Qt::PlainText);
        addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        addressRow->addWidget(addressLabel);
        auto copyButton = new QToolButton;
        copyButton->setIcon(materialIcon("copy", color));
        copyButton->setIconSize(QSize(14, 14));
        copyButton->setAutoRaise(true);
        copyButton->setCursor(Qt::PointingHandCursor);
        copyButton->setToolTip(tr("Copy address"));
        connect(copyButton, &QToolButton::clicked, this,
                [this, address] { session_.copyAddress(address); });
        addressRow->addWidget(copyButton);
        addressRow->addStretch();
        infoCol->addLayout(addressRow);
        cardRow->addLayout(infoCol, 1);
        auto write = new QPushButton(tr("Write"));
        write->setObjectName("writeToContactButton");
        write->setIcon(materialIcon("compose", color));
        write->setCursor(Qt::PointingHandCursor);
        connect(write, &QPushButton::clicked, this, [this, address] { compose({{"to", address}}); });
        cardRow->addWidget(write);
        auto rename = new QToolButton;
        rename->setObjectName("renameContactButton");
        rename->setIcon(materialIcon("edit", color));
        rename->setAutoRaise(true);
        rename->setCursor(Qt::PointingHandCursor);
        rename->setToolTip(tr("Rename"));
        connect(rename, &QToolButton::clicked, this, [this, address, label] {
            if (editContact(address, label))
                QTimer::singleShot(0, this, [this] { refreshContacts(); });
        });
        cardRow->addWidget(rename);
        auto remove = new QToolButton;
        remove->setObjectName("deleteContactButton");
        remove->setIcon(materialIcon("delete", color));
        remove->setAutoRaise(true);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(tr("Delete"));
        connect(remove, &QToolButton::clicked, this, [this, address, label] {
            if (QMessageBox::question(this, tr("Delete contact"),
                                      tr("Remove “%1” from contacts? Letters to and from "
                                         "this address are kept.")
                                          .arg(label)) != QMessageBox::Yes)
                return;
            session_.removeContact(address);
            QTimer::singleShot(0, this, [this] { refreshContacts(); });
        });
        cardRow->addWidget(remove);
        contactLayout_->addWidget(card);
    }
    if (shown == 0) {
        auto empty = new QLabel(
            all.isEmpty() ? tr("No contacts yet.\n\nAdd one with “Add contact…” above, or with the "
                            "person-plus button beside an address in a letter you are reading.")
                          : tr("No contact matches “%1”.").arg(filter));
        empty->setObjectName("contactsEmpty");
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignHCenter);
        empty->setStyleSheet("color:palette(mid);padding:24px;");
        contactLayout_->addWidget(empty);
    }
    contactLayout_->addStretch();
}
} // namespace bm
