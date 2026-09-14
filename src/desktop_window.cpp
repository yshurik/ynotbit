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
class Composer : public QDialog {
    Session &session_;
    QString id_, original_;
    QComboBox *sender_;
    QLineEdit *to_, *subject_;
    QCheckBox *broadcast_;
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
                                      broadcast_->isChecked() ? "broadcast" : "direct");
        if (id.isEmpty()) {
            status_->setText(session_.error());
            return false;
        }
        id_ = id;
        dirty_ = false;
        status_->setText("Saved in your encrypted mailbox");
        return true;
    }

  public:
    Composer(Session &session, QVariantMap letter, bool reply, QWidget *parent)
        : QDialog(parent), session_(session) {
        setObjectName("composer");
        setWindowTitle(reply ? "Reply" : "Write a letter");
        resize(740, 650);
        setModal(true);
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(12);
        sender_ = new QComboBox;
        sender_->setFont(addressFont());
        sender_->setObjectName("senderSelector");
        for (auto value : session.identities()) {
            auto identity = value.toMap();
            sender_->addItem(identity["label"].toString(), identity["address"]);
        }
        auto from = letter[reply ? "to" : "from"].toString();
        int n = sender_->findData(from);
        if (n >= 0)
            sender_->setCurrentIndex(n);
        layout->addWidget(sender_);
        broadcast_ = new QCheckBox("Broadcast to subscribers");
        layout->addWidget(broadcast_);
        broadcast_->setChecked(!reply && letter["kind"] == "broadcast");
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
        layout->addWidget(tools);
        body_ = new QTextEdit;
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
        layout->addWidget(body_, 1);
        auto format = [&](QString name, std::function<void()> fn) {
            auto a = tools->addAction(name);
            connect(a, &QAction::triggered, this, [this, fn] {
                fn();
                body_->setFocus();
            });
            return a;
        };
        format("Bold", [this] {
            QTextCharFormat f;
            f.setFontWeight(body_->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
            body_->mergeCurrentCharFormat(f);
        })->setShortcut(QKeySequence::Bold);
        format("Italic", [this] {
            QTextCharFormat f;
            f.setFontItalic(!body_->fontItalic());
            body_->mergeCurrentCharFormat(f);
        })->setShortcut(QKeySequence::Italic);
        for (auto label : {QString("Heading"), QString("Body")})
            format(label, [this, label] {
                auto c = body_->textCursor();
                auto f = c.blockFormat();
                f.setHeadingLevel(label == "Heading" ? 2 : 0);
                c.setBlockFormat(f);
                QTextCharFormat t;
                t.setFontWeight(label == "Heading" ? QFont::Bold : QFont::Normal);
                t.setFontPointSize(label == "Heading" ? 18 : 12);
                c.mergeCharFormat(t);
            });
        format("• List", [this] {
            QTextListFormat f;
            f.setStyle(QTextListFormat::ListDisc);
            body_->textCursor().createList(f);
        });
        format("1. List", [this] {
            QTextListFormat f;
            f.setStyle(QTextListFormat::ListDecimal);
            body_->textCursor().createList(f);
        });
        format("Quote", [this] {
            auto c = body_->textCursor();
            auto f = c.blockFormat();
            f.setProperty(QTextFormat::BlockQuoteLevel, 1);
            f.setLeftMargin(24);
            c.setBlockFormat(f);
        });
        format("Code", [this] {
            QTextCharFormat f;
            f.setFontFixedPitch(true);
            f.setFontFamilies({"monospace"});
            body_->mergeCurrentCharFormat(f);
        });
        format("Link", [this] {
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
        status_ = new QLabel("Drafts are saved as you write");
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto note = new QLabel(
            "Send queues proof of work and network delivery. Locking pauses preparation.");
        note->setWordWrap(true);
        layout->addWidget(note);
        auto actions = new QHBoxLayout;
        layout->addLayout(actions);
        button("Save & close", actions, [this] {
            dirty_ = true;
            if (save())
                accept();
        });
        actions->addStretch();
        auto send = button("Send letter", actions, [this] {
            dirty_ = true;
            if (save() && session_.sendLetter(id_))
                accept();
            else
                status_->setText(session_.error());
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
        connect(broadcast_, &QCheckBox::toggled, this, [this, changed](bool checked) {
            to_->setVisible(!checked);
            changed();
        });
        to_->setVisible(!broadcast_->isChecked());
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
    read->setContentsMargins(28, 28, 28, 24);
    read->setSpacing(14);
    subject_ = new QLabel("No letter selected");
    subject_->setWordWrap(true);
    subject_->setMaximumHeight(90);
    subject_->setTextFormat(Qt::PlainText);
    subject_->setStyleSheet("font-size:22px;font-weight:600;");
    read->addWidget(subject_);
    actions_ = new QWidget;
    auto actions = new QHBoxLayout(actions_);
    actions->setContentsMargins(0, 0, 0, 0);
    button("Edit / Send", actions, [this] { compose(selected_); })->setObjectName("editAction");
    button("Reply", actions, [this] { compose(selected_, true); })->setObjectName("replyAction");
    button("Archive", actions,
           [this] { session_.moveLetter(selected_["hash"].toString(), "Archive"); });
    button("Trash", actions,
           [this] { session_.moveLetter(selected_["hash"].toString(), "Trash"); });
    auto more = new QPushButton("More");
    actions->addWidget(more);
    auto menu = new QMenu(more);
    more->setMenu(menu);
    menu->addAction("Restore", this,
                    [this] { session_.restoreLetter(selected_["hash"].toString()); });
    menu->addAction("Delete permanently", this,
                    [this] { session_.deleteLetter(selected_["hash"].toString()); });
    menu->addAction("Retry", this, [this] { session_.retryLetter(selected_["hash"].toString()); });
    menu->addAction("Cancel delivery", this,
                    [this] { session_.cancelLetter(selected_["hash"].toString()); });
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
            "QPushButton,QLineEdit,QComboBox{padding:9px;border:1px solid %4;border-radius:6px;} "
            "QPushButton:hover{background:%2;} QListView,QTextEdit{border:0;} "
            "QListWidget::item{padding:10px;} QListWidget::item:selected{background:%5;color:%6;} "
            "QToolBar{border:0;spacing:3px;}")
            .arg(dark ? "#141b23" : "#f5f7fa", dark ? "#17212b" : "#f1f5f7",
                 dark ? "#1b2530" : "#ffffff", dark ? "#354553" : "#dbe3e8",
                 dark ? "#234b46" : "#dcebe8", dark ? "#73d8c7" : "#126d65"));
    letters_->viewport()->update();
    updateDeliveryStatus();
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
    subject_->setText(singleLine(selected_["subject"].toString()).left(240));
    subject_->setFont(subject_->text().contains("BM-") ? addressFont() : QApplication::font());
    fromAddress_->setText(selected_["from"].toString());
    toAddress_->setText(selected_["to"].toString());
    deliveryError_->setText(selected_["deliveryError"].toString());
    deliveryError_->setVisible(!deliveryError_->text().isEmpty());
    updateDeliveryStatus();
    details_->show();
    updateTimeline();
    body_->document()->setLayoutEnabled(false);
    body_->document()->setMarkdown(
        selected_["body"].toString(),
        QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                        QTextDocument::MarkdownNoHTML));
    body_->document()->clearUndoRedoStacks();
    for (auto block = body_->document()->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        format.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
        format.setBottomMargin(block.textList() ? 3 : 10);
        cursor.setBlockFormat(format);
    }
    body_->document()->setLayoutEnabled(true);
    body_->verticalScrollBar()->setValue(0);
    actions_->show();
    findChild<QPushButton *>("editAction")->setVisible(selected_["folder"] == "Drafts");
    findChild<QPushButton *>("replyAction")->setVisible(selected_["folder"] != "Drafts");
    session_.readLetter(id);
}
void DesktopWindow::compose(QVariantMap letter, bool reply) {
    if (!session_.mailboxOpen())
        return;
    if (letter.contains("hash"))
        letter = session_.message(letter["hash"].toString());
    Composer dialog(session_, letter, reply, this);
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
