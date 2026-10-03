#include "feed_view.h"
#include "letter_render.h"
#include "session.h"
#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QtWidgets>
#include <cmath>

namespace bm {
namespace {
// A post's body: the reader's rendering, as tall as its text, never scrolling
// on its own -- the wheel goes to the feed.
class FeedBody : public LetterView {
  public:
    explicit FeedBody(QWidget *parent = nullptr) : LetterView(parent) {
        setDocument(new SafeDocument(this));
        document()->setDocumentMargin(0); // lines up with the title above
        new AddressHighlighter(document());
        setOpenLinks(false);
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); // hex dumps don't wrap
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Text on the card itself, not in a box of its own.
        setStyleSheet("QTextBrowser{background:transparent;}");
        viewport()->setAutoFillBackground(false);
        connect(document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { fit(); });
    }
    void fit() {
        const auto bar = horizontalScrollBar();
        const int h = int(std::ceil(document()->size().height())) + 2 * frameWidth() +
                      (bar->maximum() > 0 ? bar->sizeHint().height() : 0);
        if (h != height())
            setFixedHeight(h);
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        LetterView::resizeEvent(event);
        fit();
    }
    void wheelEvent(QWheelEvent *event) override {
        event->ignore();
    }
};
QToolButton *actionButton(const QString &name, const QString &icon, const QString &tip,
                          bool dark) {
    auto b = new QToolButton;
    b->setObjectName(name);
    b->setIcon(materialIcon(icon, iconColor(dark)));
    b->setIconSize(QSize(18, 18));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
} // namespace

QString relativeTime(const QDateTime &when, const QDateTime &now) {
    const auto secs = when.secsTo(now);
    if (secs < 60)
        return FeedView::tr("just now");
    if (secs < 3600)
        return FeedView::tr("%1 min").arg(secs / 60);
    if (secs < 86400)
        return FeedView::tr("%1 h").arg(secs / 3600);
    if (when.date() == now.date().addDays(-1))
        return FeedView::tr("Yesterday");
    return QLocale().toString(when.date(), QLocale::ShortFormat);
}

FeedView::FeedView(Session &session, QWidget *parent) : QWidget(parent), session_(session) {
    setObjectName("feedView");
    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 0);
    outer->setSpacing(8);
    // Whose feed this is.
    auto header = new QHBoxLayout;
    header->setSpacing(10);
    headerIcon_ = new QLabel;
    headerIcon_->setFixedSize(28, 28);
    header->addWidget(headerIcon_);
    auto names = new QVBoxLayout;
    names->setSpacing(0);
    headerName_ = new QLabel;
    headerName_->setObjectName("feedHeaderName");
    headerName_->setTextFormat(Qt::PlainText);
    headerName_->setStyleSheet("font-size:18px;font-weight:600;");
    names->addWidget(headerName_);
    headerAddress_ = new QLabel;
    headerAddress_->setObjectName("feedHeaderAddress");
    headerAddress_->setFont(addressFont());
    headerAddress_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerAddress_->setStyleSheet("color:palette(mid);font-size:11px;");
    names->addWidget(headerAddress_);
    header->addLayout(names);
    headerCopy_ = actionButton("feedHeaderCopy", "copy", tr("Copy address"), dark_);
    connect(headerCopy_, &QToolButton::clicked, this, [this] { session_.copyAddress(address_); });
    header->addWidget(headerCopy_, 0, Qt::AlignTop);
    header->addStretch();
    outer->addLayout(header);

    scroll_ = new QScrollArea;
    scroll_->setObjectName("feedScroll");
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    cardsHolder_ = new QWidget;
    cards_ = new QVBoxLayout(cardsHolder_);
    cards_->setContentsMargins(0, 0, 8, 16);
    cards_->setSpacing(12);
    empty_ = new QLabel;
    empty_->setObjectName("feedEmpty");
    empty_->setWordWrap(true);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setStyleSheet("color:palette(mid);padding:40px;");
    empty_->hide();
    cards_->addWidget(empty_);
    cards_->addStretch();
    scroll_->setWidget(cardsHolder_);
    outer->addWidget(scroll_, 1);
    // Infinite scroll: the next page comes in before the end is reached.
    connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (hasMore_ && value >= scroll_->verticalScrollBar()->maximum() - 600)
            loadMore();
    });
    readTimer_ = new QTimer(this);
    readTimer_->setInterval(250);
    connect(readTimer_, &QTimer::timeout, this, &FeedView::trackReading);
    readTimer_->start();
    updateHeader();
}
void FeedView::setDark(bool dark) {
    if (dark == dark_)
        return;
    dark_ = dark;
    headerCopy_->setIcon(materialIcon("copy", iconColor(dark_)));
    setSource(address_, label_); // the cards' icons follow the theme
}
void FeedView::updateHeader() {
    const bool any = !address_.isEmpty();
    headerIcon_->setVisible(any);
    headerName_->setVisible(any);
    headerAddress_->setVisible(any);
    headerCopy_->setVisible(any);
    if (!any)
        return;
    headerIcon_->setPixmap(identiconPixmap(address_, 28));
    headerName_->setText(label_.isEmpty() ? address_ : label_);
    headerAddress_->setText(address_);
    headerAddress_->setVisible(headerName_->text() != address_);
}
void FeedView::clearCards() {
    for (int i = cards_->count() - 1; i >= 0; --i) {
        auto w = cards_->itemAt(i)->widget();
        if (w && w->objectName() == "feedCard") {
            cards_->takeAt(i);
            delete w;
        }
    }
    seenSince_.clear();
}
QWidget *FeedView::cardFor(const QString &hash) const {
    for (int i = 0; i < cards_->count(); ++i)
        if (auto w = cards_->itemAt(i)->widget(); w && w->property("hash").toString() == hash)
            return w;
    return nullptr;
}
void FeedView::setSource(const QString &address, const QString &label) {
    address_ = address;
    label_ = label;
    clearCards();
    hasMore_ = !address_.isEmpty();
    updateHeader();
    empty_->setText(tr("No posts from %1 yet. Broadcasts stay on the network for up to 28 days.")
                        .arg(label_.isEmpty() ? address_ : label_));
    empty_->hide();
    if (hasMore_)
        loadMore(); // shows the empty note when there is nothing
    scroll_->verticalScrollBar()->setValue(0);
}
void FeedView::loadMore() {
    if (address_.isEmpty() || !hasMore_)
        return;
    int loaded = 0;
    for (int i = 0; i < cards_->count(); ++i)
        if (auto w = cards_->itemAt(i)->widget(); w && w->objectName() == "feedCard")
            ++loaded;
    const auto rows = session_.messagePage("Broadcasts", {}, loaded, kPageSize, address_);
    hasMore_ = rows.size() == kPageSize;
    for (const auto &row : rows) {
        const auto hash = row.toMap()["hash"].toString();
        if (cardFor(hash))
            continue;
        cards_->insertWidget(cards_->count() - 1, makeCard(session_.message(hash)));
    }
    empty_->setVisible(loaded == 0 && rows.isEmpty());
    QTimer::singleShot(0, this, &FeedView::fillViewport);
}
void FeedView::fillViewport() {
    // Too few posts to scroll: nothing would ever ask for the next page.
    if (hasMore_ && scroll_->verticalScrollBar()->maximum() <= 0)
        loadMore();
}
void FeedView::refresh() {
    if (address_.isEmpty())
        return;
    QStringList shown;
    for (int i = 0; i < cards_->count(); ++i)
        if (auto w = cards_->itemAt(i)->widget(); w && w->objectName() == "feedCard")
            shown << w->property("hash").toString();
    const auto rows = session_.messagePage("Broadcasts", {}, 0,
                                           qMax(int(shown.size()), kPageSize), address_);
    QStringList current;
    QHash<QString, bool> unread;
    for (const auto &row : rows) {
        const auto m = row.toMap();
        current << m["hash"].toString();
        unread[m["hash"].toString()] = m["unread"].toBool();
    }
    // Archived, deleted or otherwise gone.
    for (const auto &hash : shown)
        if (!current.contains(hash))
            if (auto card = cardFor(hash)) {
                cards_->removeWidget(card);
                seenSince_.remove(hash);
                card->deleteLater();
            }
    // New posts: everything above the newest card already shown.
    const auto top = shown.isEmpty() ? QString() : shown.first();
    QStringList fresh;
    for (const auto &hash : current) {
        if (hash == top || shown.contains(hash))
            break;
        fresh << hash;
    }
    if (!fresh.isEmpty()) {
        // Keep the post being read where it is on screen.
        const auto bar = scroll_->verticalScrollBar();
        QWidget *anchor = nullptr;
        int anchorOffset = 0;
        if (bar->value() > 0)
            for (int i = 0; i < cards_->count(); ++i)
                if (auto w = cards_->itemAt(i)->widget();
                    w && w->objectName() == "feedCard" && w->geometry().bottom() > bar->value()) {
                    anchor = w;
                    anchorOffset = w->y() - bar->value();
                    break;
                }
        int at = cards_->indexOf(empty_) + 1;
        for (const auto &hash : fresh)
            cards_->insertWidget(at++, makeCard(session_.message(hash)));
        empty_->hide();
        if (anchor) {
            cards_->activate();
            cardsHolder_->adjustSize();
            bar->setValue(anchor->y() - anchorOffset);
        }
    }
    const auto now = QDateTime::currentDateTime();
    for (int i = 0; i < cards_->count(); ++i) {
        auto w = cards_->itemAt(i)->widget();
        if (!w || w->objectName() != "feedCard")
            continue;
        const auto hash = w->property("hash").toString();
        if (unread.contains(hash))
            setUnread(w, unread[hash]);
        if (auto time = w->findChild<QLabel *>("feedTime"))
            time->setText(relativeTime(w->property("storedAt").toDateTime(), now));
    }
}
void FeedView::setUnread(QWidget *card, bool unread) {
    card->setProperty("unread", unread);
    // Unread: a dot beside the time, as feeds mark new posts.
    if (auto dot = card->findChild<QLabel *>("feedUnreadDot"))
        dot->setVisible(unread);
}
void FeedView::trackReading() {
    if (!isVisible())
        return;
    const auto viewport = scroll_->viewport();
    const QRect seen(QPoint(0, 0), viewport->size());
    const auto now = QDateTime::currentDateTime();
    for (int i = 0; i < cards_->count(); ++i) {
        auto card = cards_->itemAt(i)->widget();
        if (!card || card->objectName() != "feedCard" || !card->property("unread").toBool())
            continue;
        const auto hash = card->property("hash").toString();
        const QRect area(card->mapTo(viewport, QPoint(0, 0)), card->size());
        const int visible = area.intersected(seen).height();
        // Half the card, or half the screen for a post taller than the screen.
        if (visible < qMin(area.height(), seen.height()) / 2) {
            seenSince_.remove(hash);
            continue;
        }
        if (!seenSince_.contains(hash)) {
            seenSince_[hash] = now;
            continue;
        }
        if (seenSince_[hash].msecsTo(now) >= kReadAfterMs) {
            seenSince_.remove(hash);
            setUnread(card, false);
            session_.readLetter(hash);
        }
    }
}
QWidget *FeedView::makeCard(const QVariantMap &letter) {
    const auto hash = letter["hash"].toString();
    const auto from = letter["from"].toString();
    const auto subject = letter["subject"].toString();
    const auto text = letter["body"].toString();
    const auto storedAt = QDateTime::fromSecsSinceEpoch(letter["storedAt"].toLongLong());
    auto card = new QFrame;
    card->setObjectName("feedCard");
    card->setProperty("hash", hash);
    card->setProperty("storedAt", storedAt);
    card->setStyleSheet("QFrame#feedCard{border:1px solid palette(midlight);border-radius:10px;}");
    auto column = new QVBoxLayout(card);
    column->setContentsMargins(16, 12, 16, 8);
    column->setSpacing(8);

    auto head = new QHBoxLayout;
    head->setSpacing(8);
    auto icon = new QLabel;
    icon->setFixedSize(32, 32);
    icon->setPixmap(identiconPixmap(from, 32));
    head->addWidget(icon);
    const auto name = session_.nameFor(from);
    auto sender = new QLabel(name.isEmpty() ? from : name);
    sender->setObjectName("feedSender");
    sender->setTextFormat(Qt::PlainText);
    sender->setStyleSheet("font-weight:600;");
    if (name.isEmpty())
        sender->setFont(addressFont());
    sender->setToolTip(from);
    head->addWidget(sender);
    auto addContact = actionButton("feedAddContact", "personAdd", tr("Add to contacts"), dark_);
    addContact->setVisible(session_.contactProblem(from).isEmpty() && !session_.isContact(from));
    connect(addContact, &QToolButton::clicked, this, [this, from] { emit addContactRequested(from); });
    head->addWidget(addContact);
    head->addStretch();
    auto dot = new QLabel;
    dot->setObjectName("feedUnreadDot");
    dot->setFixedSize(8, 8);
    dot->setToolTip(tr("Unread"));
    dot->setStyleSheet("background:palette(highlight);border-radius:4px;");
    head->addWidget(dot);
    auto time = new QLabel(relativeTime(storedAt, QDateTime::currentDateTime()));
    time->setObjectName("feedTime");
    time->setToolTip(letter["received"].toString());
    time->setStyleSheet("color:palette(mid);font-size:12px;");
    head->addWidget(time);
    column->addLayout(head);
    setUnread(card, letter["unread"].toBool());

    if (!subject.trimmed().isEmpty()) {
        auto title = new QLabel(subject);
        title->setObjectName("feedSubject");
        title->setTextFormat(Qt::PlainText);
        title->setWordWrap(true);
        title->setTextInteractionFlags(Qt::TextSelectableByMouse);
        title->setStyleSheet("font-size:15px;font-weight:600;");
        column->addWidget(title);
    }
    auto body = new FeedBody;
    body->setObjectName("feedBody");
    // A feed reads in proportional type: plain posts as the reader's Text view
    // (same lines, quotes as bars); Markdown and hex as detected.
    auto mode = detectBodyView(subject, text);
    renderBody(body, text, mode == BodyView::Plain ? BodyView::Text : mode);
    connect(body, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() == "https" &&
            QMessageBox::question(this, tr("Open link"),
                                  tr("Open this link in your browser?\n%1")
                                      .arg(url.toDisplayString())) == QMessageBox::Yes)
            QDesktopServices::openUrl(url);
    });
    column->addWidget(body);

    auto actions = new QHBoxLayout;
    actions->setSpacing(2);
    auto reply = actionButton("feedReply", "reply", tr("Reply privately"), dark_);
    connect(reply, &QToolButton::clicked, this, [this, letter] { emit replyRequested(letter); });
    auto copy = actionButton("feedCopy", "copy", tr("Copy text"), dark_);
    connect(copy, &QToolButton::clicked, this, [subject, text] {
        QApplication::clipboard()->setText(subject.trimmed().isEmpty() ? text
                                                                       : subject + "\n\n" + text);
    });
    auto open = actionButton("feedOpen", "openWindow", tr("Open in new window"), dark_);
    connect(open, &QToolButton::clicked, this, [this, letter] { emit openRequested(letter); });
    auto archive = actionButton("feedArchive", "archive", tr("Archive"), dark_);
    auto trash = actionButton("feedTrash", "delete", tr("Trash"), dark_);
    for (auto [button, folder] : {std::pair{archive, QString("Archive")}, {trash, QString("Trash")}})
        connect(button, &QToolButton::clicked, this, [this, card, hash, folder] {
            session_.moveLetter(hash, folder);
            if (!session_.error().isEmpty())
                return;
            cards_->removeWidget(card);
            seenSince_.remove(hash);
            card->hide();
            card->deleteLater(); // its own button is mid-click
        });
    for (auto b : {reply, copy, open})
        actions->addWidget(b);
    actions->addStretch();
    actions->addWidget(archive);
    actions->addWidget(trash);
    column->addLayout(actions);
    return card;
}
} // namespace bm
