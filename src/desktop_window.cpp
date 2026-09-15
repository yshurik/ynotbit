#include "desktop_window.h"
#include "session.h"
#include <QDesktopServices>
#include <QTextList>
#include <QtWidgets>

namespace bm {
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
class LetterDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return {280, 94};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        p->save();
        const auto pal = o.palette;
        p->fillRect(o.rect, o.state & QStyle::State_Selected ? pal.highlight() : pal.base());
        auto text = [&](int y, QString value, bool bold, QColor color) {
            QFont font = o.font;
            if (value.contains("BM-"))
                font.setFamilies(addressFont().families());
            font.setBold(bold);
            p->setFont(font);
            p->setPen(color);
            QRect r = o.rect.adjusted(16, y, -16, 0);
            r.setHeight(24);
            p->drawText(
                r, Qt::AlignVCenter,
                QFontMetrics(font).elidedText(singleLine(value), Qt::ElideRight, r.width()));
        };
        text(10,
             (i.data(Qt::UserRole + 9).toBool() ? "• " : "") + i.data(Qt::UserRole + 4).toString(),
             true, pal.text().color());
        text(36, i.data(Qt::UserRole + 5).toString(), false, pal.placeholderText().color());
        auto state = i.data(Qt::UserRole + 7).toString();
        const auto stateColor =
            state == "acknowledged"
                ? QColor(pal.base().color().lightness() < 128 ? "#8ce0b2" : "#17643b")
                : pal.placeholderText().color();
        text(62, state.isEmpty() ? i.data(Qt::UserRole + 11).toString() : state.replace('_', ' '),
             false, stateColor);
        p->setPen(pal.mid().color());
        p->drawLine(o.rect.bottomLeft(), o.rect.bottomRight());
        p->restore();
    }
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
    }
    return QIcon(pixmap);
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

  private:
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
                QTextCharFormat t;
                t.setFontWeight(QFont::Bold);
                t.setFontPointSize(level == 1 ? 22 : level == 2 ? 18 : 15);
                c.mergeCharFormat(t);
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
        modeRow->addWidget(modePrivate_);
        modeRow->addWidget(modePublic_);
        modeRow->addStretch();
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
        layout->addWidget(sender_);
        auto updateModeLabels = [this, identities] {
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
        };
        updateModeLabels();
        connect(sender_, &QComboBox::currentIndexChanged, this, updateModeLabels);
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
        doc->setMarkdown(original_,
                         QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                         QTextDocument::MarkdownNoHTML));
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
class MessageWindow : public QDialog {
  public:
    MessageWindow(Session &session, QVariantMap letter, bool dark, QWidget *parent)
        : QDialog(parent), session_(session), letter_(std::move(letter)), dark_(dark) {
        setObjectName("messageWindow");
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(singleLine(letter_["subject"].toString()).left(80));
        resize(640, 560);
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(3, 3, 3, 3);
        layout->setSpacing(12);
        auto toolbar = new QToolBar;
        toolbar->setObjectName("windowActionsToolbar");
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setIconSize(QSize(40, 40));
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
        layout->addWidget(toolbar);
        auto subject = subjectArea(layout, "windowSubjectLabel", "windowSubjectScroll");
        const auto subjectText = singleLine(letter_["subject"].toString());
        subject->setText(subjectText);
        subject->setFont(subjectText.contains("BM-") ? addressFont() : QApplication::font());
        auto addresses =
            new QLabel(letter_["from"].toString() + "  →  " + letter_["to"].toString());
        addresses->setObjectName("windowAddresses");
        addresses->setFont(addressFont());
        addresses->setTextFormat(Qt::PlainText);
        addresses->setWordWrap(true);
        addresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(addresses);
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
        layout->addWidget(body, 1);
        renderMarkdown(body, letter_["body"].toString());
    }

  private:
    Session &session_;
    QVariantMap letter_;
    bool dark_;
};
} // namespace
DesktopWindow::DesktopWindow(Session &session) : session_(session) {
    setObjectName("desktopWindow");
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
    auto logo = new QLabel("y");
    logo->setStyleSheet("background:#126d65;color:white;border-radius:10px;font-size:28px;font-"
                        "weight:700;padding:6px 12px;");
    top->addWidget(logo);
    auto brand = new QLabel("<b style='font-size:20px'>ynotbit</b><br><span "
                            "style='font-size:10px'>PRIVATE CORRESPONDENCE</span>");
    top->addWidget(brand);
    top->addStretch();
    button("Unlock vault", top, [this] {
        if (session_.unlocked())
            session_.lock();
        else
            session_.beginVaultUnlock();
    })->setObjectName("lockButton");
    outer->addWidget(header);
    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setContentsMargins(20, 8, 20, 8);
    outer->addWidget(error_);
    auto split = new QSplitter;
    outer->addWidget(split, 1);
    auto side = new QWidget;
    side->setObjectName("sidebar");
    auto nav = new QVBoxLayout(side);
    nav->setContentsMargins(16, 16, 16, 16);
    button("＋  Write a letter", nav, [this] {
        if (folders_->currentRow() == 4 && channels_->currentIndex() >= 0)
            compose({{"to", channels_->currentData()}, {"from", channels_->currentData()}});
        else
            compose();
    })->setObjectName("writeButton");
    nav->addSpacing(20);
    nav->addWidget(new QLabel("MAILBOX"));
    folders_ = new QListWidget;
    folders_->setObjectName("folders");
    folders_->addItems({"Inbox", "Drafts", "Outbox", "Sent", "Channels", "Broadcasts", "Archive",
                        "Trash", "Identities"});
    nav->addWidget(folders_, 1);
    nav->addWidget(new QLabel("Your keys. Your mailbox."));
    split->addWidget(side);
    auto middle = new QWidget;
    auto mid = new QVBoxLayout(middle);
    mid->setContentsMargins(12, 20, 12, 0);
    heading_ = new QLabel("Inbox");
    heading_->setStyleSheet("font-size:24px;font-weight:600;");
    mid->addWidget(heading_);
    document_ = new QLabel;
    mid->addWidget(document_);
    channelControls_ = new QWidget;
    auto channelLayout = new QVBoxLayout(channelControls_);
    channelLayout->setContentsMargins(0, 0, 0, 0);
    channels_ = new QComboBox;
    channels_->setFont(addressFont());
    channels_->setObjectName("channelSelector");
    channels_->setPlaceholderText("No channels yet");
    channels_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    channels_->setMinimumContentsLength(12);
    channels_->setAccessibleName("Channel");
    channelLayout->addWidget(channels_);
    button("Join or create channel…", channelLayout, [this] {
        session_.joinChannel();
        refreshChannels();
    });
    mid->addWidget(channelControls_);
    auto search = search_ = new QLineEdit;
    search->setPlaceholderText("Search this folder");
    mid->addWidget(search);
    auto debounce = new QTimer(this);
    debounce->setSingleShot(true);
    connect(search, &QLineEdit::textChanged, this, [debounce] { debounce->start(250); });
    connect(debounce, &QTimer::timeout, this,
            [this, search] { session_.messageModel()->setSearch(search->text()); });
    connect(channels_, &QComboBox::currentIndexChanged, this, [this] {
        session_.messageModel()->setChannel(channels_->currentData().toString());
        channels_->setToolTip("<pre>" + channels_->currentData().toString().toHtmlEscaped() +
                              "</pre>");
        updateState();
    });
    letters_ = new QListView;
    letters_->setObjectName("letters");
    letters_->setModel(session_.messageModel());
    letters_->setItemDelegate(new LetterDelegate(letters_));
    letters_->setUniformItemSizes(true);
    letters_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    letters_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mid->addWidget(letters_, 1);
    split->addWidget(middle);
    reader_ = new QWidget;
    auto read = new QVBoxLayout(reader_);
    read->setContentsMargins(3, 3, 3, 3);
    read->setSpacing(14);
    auto toolbar = new QToolBar;
    toolbar->setObjectName("actionsToolbar");
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(40, 40));
    actions_ = toolbar;
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
    read->addWidget(actions_);
    subject_ = subjectArea(read, "subjectLabel", "subjectScroll");
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
    read->addWidget(details_);
    body_ = new QTextBrowser;
    body_->setObjectName("readerBody");
    body_->setDocument(new SafeDocument(body_));
    new AddressHighlighter(body_->document());
    body_->setOpenLinks(false);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    read->addWidget(body_, 1);
    connect(body_, &QTextBrowser::anchorClicked, this, [this](QUrl url) {
        if (url.scheme() == "https" &&
            QMessageBox::question(this, "Open link",
                                  "Open this link in your browser?\n" + url.toDisplayString()) ==
                QMessageBox::Yes)
            QDesktopServices::openUrl(url);
    });
    welcome_ = new QWidget;
    auto welcomeLayout = new QVBoxLayout(welcome_);
    welcomeLayout->addStretch();
    auto welcomeText =
        new QLabel("Your keys. Your mailbox.\n\nUnlock your vault to read encrypted "
                   "correspondence.\nThe relay can keep receiving while your vault is locked.");
    welcomeText->setWordWrap(true);
    welcomeLayout->addWidget(welcomeText);
    button("Open vault…", welcomeLayout, [this] {
        session_.beginVaultOpen();
    })->setObjectName("openVaultButton");
    button("Create vault…", welcomeLayout, [this] {
        session_.beginVaultCreate();
    })->setObjectName("createVaultButton");
    button("Open mailbox…", welcomeLayout, [this] {
        session_.openMailbox();
    })->setObjectName("openMailboxButton");
    button("Create mailbox…", welcomeLayout, [this] {
        session_.createMailbox();
    })->setObjectName("createMailboxButton");
    welcomeLayout->addStretch();
    read->addWidget(welcome_, 1);
    identities_ = new QWidget;
    identityLayout_ = new QVBoxLayout(identities_);
    read->addWidget(identities_);
    split->addWidget(reader_);
    split->setSizes({212, 300, 648});
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
    // Let the controller leave its guarded operation before the modal dialog
    // submits a password through that same controller.
    connect(&session_, &Session::vaultPasswordRequired, this, &DesktopWindow::vaultDialog,
            Qt::QueuedConnection);
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
        if (i.isValid())
            (new MessageWindow(session_, session_.message(i.data(Qt::UserRole + 1).toString()),
                               appearance_.dark(), this))
                ->show();
    });
    connect(session_.messageModel(), &QAbstractItemModel::modelReset, this, [this] {
        selected_.clear();
        body_->clear();
        subject_->setText("No letter selected");
        clearDetails();
        actions_->hide();
    });
    connect(folders_, &QListWidget::currentTextChanged, this, [this](QString folder) {
        heading_->setText(folder);
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
            "QToolBar{border:0;spacing:3px;}")
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
}
void DesktopWindow::clearDetails() {
    fromAddress_->clear();
    toAddress_->clear();
    deliveryStatus_->clear();
    deliveryError_->clear();
    timeline_->clear();
    details_->hide();
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
    document_->setText(session_.document());
    error_->setText(session_.error());
    error_->setVisible(!session_.error().isEmpty());
    findChild<QPushButton *>("lockButton")
        ->setText(session_.unlocked() ? "Lock vault" : "Unlock vault");
    const bool channelPage = folders_->currentRow() == 4;
    auto write = findChild<QPushButton *>("writeButton");
    write->setText(channelPage ? "＋  Write to channel" : "＋  Write a letter");
    write->setEnabled(session_.mailboxOpen() && (!channelPage || channels_->currentIndex() >= 0));
    channelControls_->setVisible(channelPage);
    channelControls_->setEnabled(session_.unlocked());
    search_->setPlaceholderText(channelPage ? "Search this channel" : "Search this folder");
    const auto identities = session_.unlocked() ? session_.identities() : QVariantList();
    if (identities != channelIdentities_) {
        channelIdentities_ = identities;
        refreshChannels();
    }
    const bool identityPage = folders_->currentRow() == 8 && session_.unlocked();
    welcome_->setVisible(!session_.mailboxOpen() && !identityPage);
    body_->setVisible(session_.mailboxOpen() && !identityPage);
    identities_->setVisible(identityPage);
    findChild<QPushButton *>("openMailboxButton")->setVisible(session_.unlocked());
    findChild<QPushButton *>("createMailboxButton")->setVisible(session_.unlocked());
    findChild<QPushButton *>("openVaultButton")->setVisible(!session_.unlocked());
    findChild<QPushButton *>("createVaultButton")->setVisible(!session_.unlocked());
    status_->setText(
        session_.status() + " · " + QString::number(session_.objectCount()) + " cached objects · " +
        QString::number(session_.cacheBytes() / 1048576.0, 'f', 1) + " MB\n" + session_.activity());
}
void DesktopWindow::refreshChannels() {
    const auto entries = session_.channels();
    const auto previous = channels_->currentData().toString();
    QSignalBlocker blocker(channels_);
    channels_->clear();
    for (auto v : entries) {
        auto item = v.toMap();
        auto address = item["address"].toString();
        auto label = item["label"].toString();
        channels_->addItem(label == address ? address : label + " · " + address.right(8), address);
        channels_->setItemData(channels_->count() - 1, "<pre>" + address.toHtmlEscaped() + "</pre>",
                               Qt::ToolTipRole);
    }
    int index = channels_->findData(previous);
    if (index >= 0)
        channels_->setCurrentIndex(index);
    channels_->setToolTip("<pre>" + channels_->currentData().toString().toHtmlEscaped() + "</pre>");
    session_.messageModel()->setChannel(channels_->currentData().toString());
}
void DesktopWindow::selectMessage(const QString &id) {
    try {
        selected_ = session_.message(id);
    } catch (const std::exception &e) {
        error_->setText(e.what());
        error_->show();
        return;
    }
    const auto fullSubject = singleLine(selected_["subject"].toString());
    subject_->setText(fullSubject);
    subject_->setFont(fullSubject.contains("BM-") ? addressFont() : QApplication::font());
    fromAddress_->setText(selected_["from"].toString());
    toAddress_->setText(selected_["to"].toString());
    deliveryError_->setText(selected_["deliveryError"].toString());
    deliveryError_->setVisible(!deliveryError_->text().isEmpty());
    updateDeliveryStatus();
    details_->show();
    updateTimeline();
    renderMarkdown(body_, selected_["body"].toString());
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
void DesktopWindow::compose(QVariantMap letter, bool reply) {
    if (!session_.mailboxOpen())
        return;
    if (letter.contains("hash"))
        letter = session_.message(letter["hash"].toString());
    Composer dialog(session_, letter, reply, appearance_.dark(), this);
    dialog.exec();
}
void DesktopWindow::vaultDialog(QString path, bool create) {
    QDialog dialog(this);
    dialog.setObjectName("vaultPasswordDialog");
    dialog.setWindowTitle(create ? "Create encrypted vault" : "Unlock vault");
    dialog.setMinimumWidth(440);
    auto layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    auto label = new QLabel(path);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    auto password = new QLineEdit;
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText("Password");
    layout->addWidget(password);
    auto repeat = new QLineEdit;
    repeat->setEchoMode(QLineEdit::Password);
    repeat->setPlaceholderText("Repeat password");
    repeat->setVisible(create);
    layout->addWidget(repeat);
    auto error = new QLabel;
    error->setWordWrap(true);
    layout->addWidget(error);
    auto controls = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    controls->button(QDialogButtonBox::Ok)->setText(create ? "Create vault" : "Unlock");
    layout->addWidget(controls);
    connect(controls, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(&session_, &Session::vaultPasswordAccepted, &dialog, &QDialog::accept);
    connect(controls, &QDialogButtonBox::accepted, &dialog, [&] {
        session_.submitVaultPassword(password->text(), repeat->text(), create);
        error->setText(session_.error());
    });
    dialog.exec();
    password->clear();
    repeat->clear();
}
void DesktopWindow::refreshIdentities() {
    while (auto item = identityLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (auto v : session_.identities()) {
        auto m = v.toMap();
        auto name = new QLabel(m["label"].toString());
        if (name->text().contains("BM-"))
            name->setFont(addressFont());
        name->setTextFormat(Qt::PlainText);
        identityLayout_->addWidget(name);
        auto label = new QLabel(m["address"].toString());
        label->setFont(addressFont());
        label->setObjectName("identityAddress");
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        identityLayout_->addWidget(label);
        button("Write", identityLayout_, [this, m] { compose({{"to", m["address"]}}); });
        button("Rename", identityLayout_, [this, m] {
            session_.renameIdentity(m["address"].toString());
            refreshIdentities();
        });
    }
    identityLayout_->addStretch();
}
} // namespace bm
