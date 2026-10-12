#include "chats_pane.h"
#include "letter_render.h"
#include <QtWidgets>
#include <QTextDocument>

namespace bm {
namespace {
QFont scaled(QFont font, double factor) {
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(9, qRound(font.pixelSize() * factor)));
    else
        font.setPointSizeF(font.pointSizeF() * factor);
    return font;
}
QString tickName(ChatTick tick) {
    switch (tick) {
    case ChatTick::Pending:
        return "pending";
    case ChatTick::Sent:
        return "sent";
    case ChatTick::Delivered:
        return "delivered";
    case ChatTick::Failed:
        return "failed";
    case ChatTick::None:
        break;
    }
    return {};
}
QString tickText(ChatTick tick) {
    switch (tick) {
    case ChatTick::Pending:
        return ChatsPane::tr("Waiting to be sent");
    case ChatTick::Sent:
        return ChatsPane::tr("Sent to the network");
    case ChatTick::Delivered:
        return ChatsPane::tr("Delivered");
    case ChatTick::Failed:
        return ChatsPane::tr("Not delivered");
    case ChatTick::None:
        break;
    }
    return {};
}
// Delivered is the accent, a failure red, the rest quiet: the theme's own colours (the
// palette's Link and PlaceholderText), by the theme, which is known while a palette
// may still be changing.
QColor tickColor(ChatTick tick, bool dark) {
    if (tick == ChatTick::Delivered)
        return QColor(dark ? "#73d8c7" : "#126d65");
    if (tick == ChatTick::Failed)
        return QColor(dark ? "#ff8a80" : "#c62828");
    return QColor(dark ? "#a5b4c1" : "#586c7b");
}
// The tick as a picture: a clock, a check, two checks, or a ring with a stroke.
QPixmap tickPixmap(ChatTick tick, const QColor &color, int size, qreal ratio) {
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 16.0, size / 16.0);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const auto check = [&p](double dx) {
        QPainterPath path;
        path.moveTo(1.8 + dx, 8.6);
        path.lineTo(5 + dx, 11.8);
        path.lineTo(10.2 + dx, 5);
        p.drawPath(path);
    };
    switch (tick) {
    case ChatTick::Pending:
        p.drawEllipse(QPointF(8, 8), 5.6, 5.6);
        p.drawLine(QPointF(8, 8), QPointF(8, 4.8));
        p.drawLine(QPointF(8, 8), QPointF(10.4, 9.4));
        break;
    case ChatTick::Sent:
        check(2);
        break;
    case ChatTick::Delivered:
        check(0);
        check(3.6);
        break;
    case ChatTick::Failed:
        p.drawEllipse(QPointF(8, 8), 5.8, 5.8);
        p.drawLine(QPointF(8, 5), QPointF(8, 8.8));
        p.drawPoint(QPointF(8, 11));
        break;
    case ChatTick::None:
        break;
    }
    return pixmap;
}
// A message's time: the clock time of one from today, the date too for an older
// one (the date alone where there is little room).
QString messageTime(qint64 seconds, bool dateAlone = false) {
    const auto when = QDateTime::fromSecsSinceEpoch(seconds);
    const QLocale locale;
    if (when.date() == QDate::currentDate())
        return locale.toString(when.time(), QLocale::ShortFormat);
    return dateAlone ? locale.toString(when.date(), QLocale::ShortFormat)
                     : locale.toString(when, QLocale::ShortFormat);
}
// A message on one line, without its Markdown.
QString plainLine(const QString &markdown) {
    QTextDocument document;
    document.setMarkdown(markdown, QTextDocument::MarkdownNoHTML);
    return document.toPlainText().simplified();
}
// The window's colours (DesktopWindow::updateTheme), by theme: the card and the chip
// are drawn in them.
struct Tones {
    QString surface, border, accent, accentHover, window, muted;
};
Tones tones(bool dark) {
    return dark ? Tones{"#1b2530", "#354553", "#73d8c7", "#8fe3d4", "#141b23", "#a5b4c1"}
                : Tones{"#ffffff", "#dbe3e8", "#126d65", "#0f5c55", "#f5f7fa", "#586c7b"};
}
// An identity's or a peer's picture, sharp at any pixel ratio.
QPixmap addressPixmap(const QString &address, int size, qreal ratio) {
    auto pixmap = identiconPixmap(address, qRound(size * ratio));
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}
} // namespace
ChatTick chatTickFor(const QString &state) {
    static const QSet<QString> pending = {"queued",          "awaiting_pubkey",
                                          "calculating_ack", "calculating_message",
                                          "ready",           "publishing"};
    if (pending.contains(state))
        return ChatTick::Pending;
    if (state == "awaiting_ack" || state == "published")
        return ChatTick::Sent;
    if (state == "acknowledged")
        return ChatTick::Delivered;
    if (state == "failed" || state == "expired" || state == "cancelled")
        return ChatTick::Failed;
    return ChatTick::None; // "received", or a state nobody sent
}
ChatRow::ChatRow(const QString &address, const QString &name, QWidget *parent)
    : QPushButton(name, parent), address_(address), name_(name) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    updateTip();
}
void ChatRow::setSummary(const QString &body, qint64 received, ChatTick tick, int unread) {
    if (body == body_ && received == received_ && tick == tick_ && unread == unread_)
        return;
    if (body != body_) {
        body_ = body;
        preview_ = plainLine(body);
    }
    received_ = received;
    tick_ = tick;
    unread_ = unread;
    update();
}
void ChatRow::setVia(const QString &address, const QString &name) {
    if (address == via_ && name == viaName_)
        return;
    via_ = address;
    viaName_ = name;
    updateTip();
    update();
}
// The peer's name above its address, and the identity the chat is under.
void ChatRow::updateTip() {
    auto tip = (name_ != address_ ? "<b>" + name_.toHtmlEscaped() + "</b>" : QString()) + "<pre>" +
               address_.toHtmlEscaped() + "</pre>";
    if (!via_.isEmpty())
        tip += ChatsPane::tr("Send as") + " <b>" + viaName_.toHtmlEscaped() + "</b>";
    setToolTip(tip);
}
void ChatRow::setCollapsed(bool collapsed) {
    if (collapsed == collapsed_)
        return;
    collapsed_ = collapsed;
    setText(collapsed ? QString() : name_); // collapsed, the name is in the tooltip
    updateGeometry();
    update();
}
QSize ChatRow::sizeHint() const {
    return collapsed_ ? QSize(40, 44) : QSize(176, 54);
}
QSize ChatRow::minimumSizeHint() const {
    return collapsed_ ? QSize(40, 44) : QSize(120, 54);
}
void ChatRow::paintEvent(QPaintEvent *) {
    QStylePainter p(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    p.drawControl(QStyle::CE_PushButton, option); // the background the style sheet gives it
    p.setRenderHint(QPainter::Antialiasing);
    const bool selected = isChecked();
    const QColor ground = palette().color(selected ? QPalette::Highlight : QPalette::Window);
    const QColor text =
        palette().color(selected ? QPalette::HighlightedText : QPalette::WindowText);
    QColor muted = selected ? text : palette().color(QPalette::PlaceholderText);
    if (selected)
        muted.setAlphaF(0.75);
    const QColor accent = palette().color(QPalette::Link);
    const QColor onAccent = accent.lightness() > 150 ? QColor("#10202b") : QColor(Qt::white);
    const qreal ratio = devicePixelRatioF();
    // The identity the chat is under, small on the corner of the peer's picture, in a ring
    // of the row's own colour.
    const auto drawVia = [&](int x, int y, int size) {
        if (via_.isEmpty())
            return;
        p.setPen(Qt::NoPen);
        p.setBrush(ground);
        p.drawRoundedRect(QRect(x - 2, y - 2, size + 4, size + 4), 3, 3);
        p.drawPixmap(x, y, addressPixmap(via_, size, ratio));
    };
    const auto count = [&] { return unread_ > 99 ? QString("99+") : QString::number(unread_); };
    if (collapsed_) {
        const int x = (width() - 26) / 2, y = (height() - 26) / 2;
        p.drawPixmap(x, y, addressPixmap(address_, 26, ratio));
        drawVia(x + 26 - 8, y + 26 - 8, 12);
        if (unread_ > 0) {
            QFont font = scaled(this->font(), 0.8);
            font.setBold(true);
            const int w = qMax(15, QFontMetrics(font).horizontalAdvance(count()) + 8);
            const QRect box(width() - w - 1, 2, w, 15);
            p.setPen(QPen(ground, 1.5));
            p.setBrush(accent);
            p.drawRoundedRect(box, 7.5, 7.5);
            p.setPen(onAccent);
            p.setFont(font);
            p.drawText(box, Qt::AlignCenter, count());
        }
        return;
    }
    const int pad = 8, size = 32, gap = 8;
    const int left = pad + size + gap, right = width() - pad;
    const int avatarTop = (height() - size) / 2;
    p.drawPixmap(pad, avatarTop, addressPixmap(address_, size, ratio));
    drawVia(pad + size - 10, avatarTop + size - 10, 15);
    QFont nameFont = font();
    nameFont.setWeight(QFont::DemiBold);
    const QFont smallFont = scaled(font(), 0.85);
    const QFontMetrics nameMetrics(nameFont), smallMetrics(smallFont), lineMetrics(font());
    const int nameHeight = nameMetrics.height(), lineHeight = lineMetrics.height();
    const bool second = !preview_.isEmpty() || unread_ > 0;
    const int top = (height() - (second ? nameHeight + 3 + lineHeight : nameHeight)) / 2;
    int edge = right;
    if (received_) {
        const auto time = messageTime(received_, true);
        const int w = smallMetrics.horizontalAdvance(time);
        edge -= w;
        p.setFont(smallFont);
        p.setPen(muted);
        p.drawText(QRect(edge, top, w, nameHeight), Qt::AlignVCenter | Qt::AlignRight, time);
    }
    if (tick_ != ChatTick::None) {
        const int s = 12;
        edge -= s + 3;
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        p.drawPixmap(edge, top + (nameHeight - s) / 2,
                     tickPixmap(tick_, tickColor(tick_, dark), s, ratio));
    }
    const int nameWidth = qMax(0, edge - 6 - left);
    p.setFont(nameFont);
    p.setPen(text);
    p.drawText(QRect(left, top, nameWidth, nameHeight), Qt::AlignVCenter | Qt::AlignLeft,
               nameMetrics.elidedText(name_, name_ == address_ ? Qt::ElideMiddle : Qt::ElideRight,
                                      nameWidth));
    if (!second)
        return;
    const int line = top + nameHeight + 3;
    int end = right;
    if (unread_ > 0) {
        QFont font = scaled(this->font(), 0.85);
        font.setBold(true);
        const int w = qMax(18, QFontMetrics(font).horizontalAdvance(count()) + 10);
        const QRect box(right - w, line + (lineHeight - 18) / 2, w, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(box, 9, 9);
        p.setPen(onAccent);
        p.setFont(font);
        p.drawText(box, Qt::AlignCenter, count());
        end = box.left() - 6;
    }
    const int previewWidth = qMax(0, end - left);
    p.setFont(font());
    p.setPen(unread_ > 0 ? text : muted);
    p.drawText(QRect(left, line, previewWidth, lineHeight), Qt::AlignVCenter | Qt::AlignLeft,
               lineMetrics.elidedText(preview_, Qt::ElideRight, previewWidth));
}
ChatsPane::ChatsPane(QWidget *parent) : QWidget(parent) {
    setObjectName("chatsPane");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 0);
    auto conversation = new QWidget;
    conversation->setObjectName("chatConversation");
    auto right = new QVBoxLayout(conversation);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);
    // The header: the peer, and at the right the identity the chat is under. Its edges
    // are the bubbles' and the composer's.
    auto header = new QWidget;
    header->setObjectName("chatHeader");
    auto head = new QHBoxLayout(header);
    head->setContentsMargins(20, 10, 20, 10);
    head->setSpacing(10);
    avatar_ = new QLabel;
    avatar_->setObjectName("chatAvatar");
    avatar_->setFixedSize(32, 32);
    head->addWidget(avatar_);
    auto names = new QVBoxLayout;
    names->setSpacing(4);
    title_ = new QLabel;
    title_->setObjectName("chatTitle");
    title_->setTextFormat(Qt::PlainText);
    title_->setStyleSheet("font-size:15px;font-weight:600;");
    names->addWidget(title_);
    address_ = new QLabel;
    address_->setTextFormat(Qt::PlainText);
    address_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    address_->setStyleSheet("color:palette(placeholder-text);font-size:12px;");
    names->addWidget(address_);
    head->addLayout(names, 1);
    identityChip_ = new QFrame;
    identityChip_->setObjectName("chatIdentity");
    auto chip = new QHBoxLayout(identityChip_);
    chip->setContentsMargins(10, 3, 12, 3);
    chip->setSpacing(6);
    auto sendAs = new QLabel(tr("Send as"));
    sendAs->setStyleSheet("color:palette(placeholder-text);font-size:11px;");
    chip->addWidget(sendAs);
    identityIcon_ = new QLabel;
    identityIcon_->setFixedSize(18, 18);
    chip->addWidget(identityIcon_);
    identityName_ = new QLabel;
    identityName_->setObjectName("chatIdentityName");
    identityName_->setTextFormat(Qt::PlainText);
    chip->addWidget(identityName_);
    head->addWidget(identityChip_, 0, Qt::AlignVCenter);
    right->addWidget(header);

    scroll_ = new QScrollArea;
    scroll_->setObjectName("chatMessagesScroll");
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The reader is at the end while the bar is. New messages are laid out later
    // than setMessages() returns, so the bar is followed as its range grows.
    auto bar = scroll_->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this,
            [this, bar](int value) { atEnd_ = value >= bar->maximum() - 4; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int maximum) {
        if (atEnd_)
            bar->setValue(maximum);
    });
    auto messageContainer = new QWidget;
    messages_ = new QVBoxLayout(messageContainer);
    messages_->setContentsMargins(20, 16, 20, 16);
    messages_->setSpacing(12);
    messages_->addStretch();
    scroll_->setWidget(messageContainer);
    welcomeTitle_ = new QLabel;
    welcomeTitle_->setParent(messageContainer);
    welcomeTitle_->setAlignment(Qt::AlignCenter);
    welcomeTitle_->setWordWrap(true);
    welcomeTitle_->setStyleSheet("font-size:16px;font-weight:600;");
    messages_->insertWidget(0, welcomeTitle_, 1);
    right->addWidget(scroll_, 1);
    // The composer: one rounded card with the field, which grows with its text, and the
    // send button, round, at its foot.
    composeBar_ = new QWidget;
    composeBar_->setObjectName("chatComposerBar");
    auto composer = new QVBoxLayout(composeBar_);
    composer->setContentsMargins(20, 6, 20, 16);
    card_ = new QFrame;
    card_->setObjectName("chatCard");
    auto cardLayout = new QHBoxLayout(card_);
    cardLayout->setContentsMargins(16, 6, 6, 6);
    cardLayout->setSpacing(8);
    input_ = new QPlainTextEdit;
    input_->setObjectName("chatComposer");
    input_->setFrameShape(QFrame::NoFrame);
    input_->document()->setDocumentMargin(0);
    input_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    input_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    input_->setPlaceholderText(tr("Write a message…"));
    input_->setAccessibleName(tr("Chat message"));
    input_->setToolTip(tr("Enter sends, Shift+Enter starts a new line"));
    input_->installEventFilter(this); // Enter sends, and the card shows the focus
    cardLayout->addWidget(input_, 1, Qt::AlignVCenter);
    send_ = new QPushButton;
    send_->setObjectName("chatSendButton");
    send_->setFixedSize(36, 36);
    send_->setIconSize(QSize(18, 18));
    send_->setCursor(Qt::PointingHandCursor);
    send_->setToolTip(tr("Send message"));
    send_->setAccessibleName(tr("Send message"));
    send_->setEnabled(false);
    cardLayout->addWidget(send_, 0, Qt::AlignBottom);
    composer->addWidget(card_);
    right->addWidget(composeBar_);
    layout->addWidget(conversation, 1);
    connect(input_, &QPlainTextEdit::textChanged, this, [this] { updateSend(); });
    connect(input_->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this] { fitComposer(); });
    connect(send_, &QPushButton::clicked, this,
            [this] { emit sendRequested(identity_, input_->toPlainText()); });
    fitComposer();
    applyTheme();
    setConversation({}, {}, {}, {}, false);
}
void ChatsPane::setConversation(const QString &identity, const QString &identityLabel,
                                const QString &peer, const QString &peerLabel, bool showIdentity) {
    contact_ = peer;
    identity_ = identity;
    const qreal ratio = devicePixelRatioF();
    title_->setText(peer.isEmpty()        ? tr("Your conversations")
                    : peerLabel.isEmpty() ? peer
                                          : peerLabel);
    address_->setText(peer);
    address_->setVisible(!peer.isEmpty());
    avatar_->setVisible(!peer.isEmpty());
    if (!peer.isEmpty())
        avatar_->setPixmap(addressPixmap(peer, 32, ratio));
    else
        avatar_->clear();
    const auto name = identityLabel.isEmpty() ? identity : identityLabel;
    identityName_->setText(name);
    if (!identity.isEmpty())
        identityIcon_->setPixmap(addressPixmap(identity, 18, ratio));
    identityChip_->setToolTip("<b>" + name.toHtmlEscaped() + "</b><pre>" +
                              identity.toHtmlEscaped() + "</pre>");
    identityChip_->setVisible(showIdentity && !identity.isEmpty() && !peer.isEmpty());
    welcomeTitle_->setText(peer.isEmpty() ? tr("Select a chat, or start a new one")
                                          : tr("No chat messages yet"));
    composeBar_->setVisible(!peer.isEmpty());
    card_->setEnabled(!identity.isEmpty());
    updateSend();
}
QString ChatsPane::contact() const {
    return contact_;
}
QString ChatsPane::identity() const {
    return identity_;
}
void ChatsPane::clearComposer() {
    input_->clear();
}
void ChatsPane::focusComposer() {
    input_->setFocus();
}
void ChatsPane::updateSend() {
    send_->setEnabled(!contact_.isEmpty() && !identity_.isEmpty() &&
                      !input_->toPlainText().trimmed().isEmpty());
}
// The field is as tall as its text (as the layout has it: a line is not always the font's
// line spacing), up to six lines; more scrolls, and only then is there a scrollbar.
void ChatsPane::fitComposer() {
    const auto layout = input_->document()->documentLayout();
    int text = 0;
    for (auto block = input_->document()->begin(); block.isValid(); block = block.next())
        text += qCeil(layout->blockBoundingRect(block).height());
    const int line = input_->fontMetrics().lineSpacing();
    input_->setFixedHeight(qBound(line, text, 6 * line));
    input_->setVerticalScrollBarPolicy(text > 6 * line ? Qt::ScrollBarAsNeeded
                                                       : Qt::ScrollBarAlwaysOff);
}
// The card takes the accent while its field has the focus. A style sheet reads a property,
// not a state, so the style is told when it changes.
void ChatsPane::showFocus(bool focused) {
    card_->setProperty("focused", focused);
    card_->style()->unpolish(card_);
    card_->style()->polish(card_);
}
// Enter sends, as in a chat; Shift+Enter is the line break. With nothing to send, Enter
// does nothing: it would only leave a blank line before the text.
bool ChatsPane::eventFilter(QObject *watched, QEvent *event) {
    if (watched == input_) {
        if (event->type() == QEvent::FocusIn) {
            showFocus(true);
        } else if (event->type() == QEvent::FocusOut) {
            showFocus(false);
        } else if (event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent *>(event);
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                !(key->modifiers() & Qt::ShiftModifier)) {
                if (send_->isEnabled())
                    send_->click();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
void ChatsPane::setMessages(const QVariantList &messages) {
    const bool sameThread = shownContact_ == contact_ && shownIdentity_ == identity_;
    // Asked for what it shows, in the same colours: nothing to redraw. A redraw
    // would drop a selection or the link under the pointer.
    if (sameThread && shownDark_ == dark_ && messages == shown_)
        return;
    shown_ = messages;
    shownContact_ = contact_;
    shownIdentity_ = identity_;
    shownDark_ = dark_;
    if (!sameThread)
        atEnd_ = true; // another conversation opens at its newest message
    while (messages_->count() > 1) {
        auto item = messages_->takeAt(0);
        if (item->widget() != welcomeTitle_) delete item->widget();
        delete item;
    }
    if (welcomeTitle_->parentWidget() != scroll_->widget())
        welcomeTitle_->setParent(scroll_->widget());
    welcomeTitle_->setVisible(messages.isEmpty());
    if (messages.isEmpty()) {
        messages_->insertWidget(0, welcomeTitle_, 1);
        return;
    }
    for (const auto &value : messages) {
        const auto item = value.toMap();
        const bool outgoing = item.value("outgoing").toBool();
        auto row = new QWidget;
        auto rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        if (outgoing) rowLayout->addStretch(1);
        auto bubble = new QWidget;
        bubble->setObjectName(outgoing ? "outgoingChatBubble" : "incomingChatBubble");
        bubble->setMaximumWidth(560);
        auto bubbleLayout = new QVBoxLayout(bubble);
        bubbleLayout->setContentsMargins(12, 8, 12, 8);
        QTextDocument document;
        document.setMarkdown(item.value("body").toString(), QTextDocument::MarkdownNoHTML);
        auto body = new QLabel(document.toHtml());
        body->setObjectName("chatBody");
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        // A wrapping label asks for a narrow, tall box. A message is as wide as its
        // text, up to what the bubble allows (560 less its margins).
        body->setMinimumWidth(qMin(qCeil(document.idealWidth()) + 2, 536));
        body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        // A stranger's link: the window asks before opening one, as for a letter.
        connect(body, &QLabel::linkActivated, this,
                [this](const QString &link) { emit linkClicked(QUrl(link)); });
        bubbleLayout->addWidget(body);
        // Under the text, at the right: its time, and for ours how far it has got.
        auto footer = new QHBoxLayout;
        footer->setContentsMargins(0, 0, 0, 0);
        footer->setSpacing(4);
        footer->addStretch(1);
        auto details = new QLabel(messageTime(item.value("received").toLongLong()));
        details->setObjectName("chatDetails");
        details->setStyleSheet("color:palette(placeholder-text);font-size:11px;");
        footer->addWidget(details);
        const auto tick = outgoing ? chatTickFor(item.value("state").toString()) : ChatTick::None;
        if (tick != ChatTick::None) {
            auto mark = new QLabel;
            mark->setObjectName("chatTick");
            mark->setProperty("tick", tickName(tick));
            mark->setToolTip(tickText(tick));
            mark->setPixmap(tickPixmap(tick, tickColor(tick, dark_), 14, devicePixelRatioF()));
            footer->addWidget(mark);
        }
        bubbleLayout->addLayout(footer);
        const auto shade = outgoing ? (dark_ ? "#174944" : "#def4ef")
                                    : (dark_ ? "#30343c" : "#f2f3f5");
        bubble->setStyleSheet(QString("border-radius:10px;background:%1;").arg(shade));
        rowLayout->addWidget(bubble);
        if (!outgoing) rowLayout->addStretch(1);
        messages_->insertWidget(messages_->count() - 1, row);
        // A layout shows a new widget only later, and a hidden one takes no room. A layout
        // pass in between, one that was already waiting, would see an empty conversation and
        // shorten the bar to nothing; the bar then reads "at the end", and a reader who had
        // scrolled up would be taken to the newest message.
        row->show();
    }
    auto bar = scroll_->verticalScrollBar();
    if (atEnd_)
        bar->setValue(bar->maximum());
}
// The card, the chip and the send button, in the window's own colours: a white field with
// a hairline, rounded, the accent on focus; the send button the accent, the window's
// colour on it (quiet when there is nothing to send).
void ChatsPane::applyTheme() {
    const auto t = tones(dark_);
    const auto sheet =
        QString("QFrame#chatCard{background:%1;border:1px solid %2;border-radius:14px;} "
                "QFrame#chatCard[focused=\"true\"]{border-color:%3;} "
                "QFrame#chatCard:disabled{background:transparent;} "
                "QPlainTextEdit#chatComposer{background:transparent;border:0;padding:0;} "
                "QPushButton#chatSendButton{background:%3;border:0;border-radius:18px;"
                "padding:0;} "
                "QPushButton#chatSendButton:hover{background:%4;} "
                "QPushButton#chatSendButton:disabled{background:%2;} "
                "QFrame#chatIdentity{background:transparent;border:1px solid %2;"
                "border-radius:13px;}")
            .arg(t.surface, t.border, t.accent, t.accentHover);
    if (styleSheet() != sheet)
        setStyleSheet(sheet);
    const qreal ratio = devicePixelRatioF();
    const auto glyph = [&](const QString &color) {
        return materialIcon("sent", QColor(color)).pixmap(QSize(18, 18), ratio);
    };
    QIcon icon;
    icon.addPixmap(glyph(t.window), QIcon::Normal);
    icon.addPixmap(glyph(t.muted), QIcon::Disabled);
    send_->setIcon(icon);
}
void ChatsPane::setDark(bool dark) {
    const bool changed = dark != dark_;
    dark_ = dark;
    applyTheme();
    if (changed)
        setMessages(shown_); // the bubbles' colours
}
} // namespace bm
