#include "pybm_import_dialog.h"
#include "appearance.h"
#include "desktop_window.h"
#include "qt_bridge.h"
#include "session.h"
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace bm {
namespace {
using bmc::pybm::Trouble;
using Phase = bmc::pybm::Progress::Phase;

QString number(qint64 n) {
    return QLocale().toString(n);
}
// Small grey text, wrapped to the width the dialog has.
QLabel *note(const QString &name, bool visible = true) {
    auto label = new QLabel;
    label->setObjectName(name);
    label->setWordWrap(true);
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(policy);
    label->setStyleSheet("color:palette(mid);font-size:12px;");
    label->setVisible(visible);
    return label;
}
QString reasonText(bmc::IdentityImport::Why why) {
    using Why = bmc::IdentityImport::Why;
    switch (why) {
    case Why::MissingKey:
        return PyBitmessageImportDialog::tr("a private key is missing");
    case Why::BadKey:
        return PyBitmessageImportDialog::tr("a private key is damaged");
    case Why::WrongKeys:
        return PyBitmessageImportDialog::tr("the keys do not belong to that address");
    case Why::Full:
        return PyBitmessageImportDialog::tr("the vault is full");
    default:
        return PyBitmessageImportDialog::tr("an address this app cannot use");
    }
}
// What went wrong, in a sentence; the technical detail goes on its own line.
QString troubleText(Trouble trouble) {
    switch (trouble) {
    case Trouble::NothingThere:
        return PyBitmessageImportDialog::tr("There is no PyBitmessage data in this folder. Choose "
                                            "the folder that holds keys.dat and messages.dat.");
    case Trouble::Busy:
        return PyBitmessageImportDialog::tr("PyBitmessage is using its data at this moment. Try "
                                            "again in a moment.");
    case Trouble::Interrupted:
        return PyBitmessageImportDialog::tr("PyBitmessage did not close its data properly. Start "
                                            "it once, close it and try again.");
    case Trouble::Failed:
        return PyBitmessageImportDialog::tr("Your mailbox or vault could not be written to.");
    default:
        return PyBitmessageImportDialog::tr("The data in this folder cannot be read.");
    }
}
} // namespace

PyBitmessageImportDialog::PyBitmessageImportDialog(Session &session, const QStringList &folders,
                                                   QWidget *parent)
    : QDialog(parent), session_(session), timer_(new QTimer(this)) {
    setObjectName("pybmImportDialog");
    setWindowTitle(tr("Import PyBitmessage data"));
    setMinimumWidth(540);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &PyBitmessageImportDialog::pump);
    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 20, 20, 20);
    pages_ = new QStackedWidget;
    pages_->setObjectName("pybmPages");
    outer->addWidget(pages_);

    // First page: where, and what.
    auto choose = new QWidget;
    auto chooseColumn = new QVBoxLayout(choose);
    chooseColumn->setContentsMargins(0, 0, 0, 0);
    chooseColumn->setSpacing(10);
    auto intro = new QLabel(tr("Bring your identities, chans, address book, subscriptions and "
                               "letters from PyBitmessage into the open vault and mailbox. "
                               "PyBitmessage's own files are only read, never changed."));
    intro->setObjectName("pybmIntro");
    intro->setWordWrap(true);
    chooseColumn->addWidget(intro);
    chooseColumn->addWidget(new QLabel(tr("PyBitmessage data folder")));
    folder_ = new QComboBox;
    folder_->setObjectName("pybmFolderCombo");
    folder_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    folder_->setMinimumContentsLength(30);
    for (const auto &path : folders)
        if (folder_->findData(path) < 0) {
            folder_->addItem(QDir::toNativeSeparators(path), path);
            folder_->setItemData(folder_->count() - 1, QDir::toNativeSeparators(path),
                                 Qt::ToolTipRole);
        }
    folder_->addItem(tr("Choose another folder…"), QString());
    chooseColumn->addWidget(folder_);
    trouble_ = note("pybmTrouble", false);
    trouble_->setStyleSheet("");
    chooseColumn->addWidget(trouble_);
    checkAgain_ = new QPushButton(tr("Check again"));
    checkAgain_->setObjectName("pybmCheckAgainButton");
    checkAgain_->hide();
    chooseColumn->addWidget(checkAgain_, 0, Qt::AlignLeft);

    auto grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);
    const auto row = [&](int at, const QString &text, const char *checkName, const char *countName,
                         QCheckBox *&check, QLabel *&count) {
        check = new QCheckBox(text);
        check->setObjectName(checkName);
        count = new QLabel;
        count->setObjectName(countName);
        count->setStyleSheet("color:palette(mid);");
        count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(check, at, 0);
        grid->addWidget(count, at, 1);
    };
    row(0, tr("Identities and chans"), "pybmIdentitiesCheck", "pybmIdentitiesCount", identities_,
        identitiesCount_);
    row(1, tr("Address book"), "pybmContactsCheck", "pybmContactsCount", contacts_, contactsCount_);
    row(2, tr("Subscriptions"), "pybmSubscriptionsCheck", "pybmSubscriptionsCount", subscriptions_,
        subscriptionsCount_);
    row(3, tr("Letters of the inbox, channels and subscriptions"), "pybmInboxCheck",
        "pybmInboxCount", inbox_, inboxCount_);
    row(4, tr("Sent letters and unsent drafts"), "pybmSentCheck", "pybmSentCount", sent_,
        sentCount_);
    row(5, tr("Deleted letters"), "pybmTrashCheck", "pybmTrashCount", trash_, trashCount_);
    grid->setColumnStretch(0, 1);
    chooseColumn->addLayout(grid);
    trashHint_ = note("pybmTrashHint", false);
    trashHint_->setText(tr("Without them, deleted letters that your node still holds can come back "
                           "into your inbox."));
    chooseColumn->addWidget(trashHint_);
    notes_ = note("pybmNotes");
    chooseColumn->addWidget(notes_);
    chooseColumn->addStretch(1);
    auto chooseButtons = new QHBoxLayout;
    chooseButtons->addStretch();
    auto cancel = new QPushButton(DesktopWindow::tr("Cancel"));
    cancel->setObjectName("pybmCancelButton");
    import_ = new QPushButton(tr("Import"));
    import_->setObjectName("pybmImportButton");
    makeDefault(import_);
    chooseButtons->addWidget(cancel);
    chooseButtons->addWidget(import_);
    chooseColumn->addLayout(chooseButtons);
    pages_->addWidget(choose);

    // Second page: how far it has got.
    auto progress = new QWidget;
    auto progressColumn = new QVBoxLayout(progress);
    progressColumn->setContentsMargins(0, 0, 0, 0);
    progressColumn->setSpacing(10);
    phase_ = new QLabel;
    phase_->setObjectName("pybmPhase");
    phase_->setWordWrap(true);
    progressColumn->addWidget(phase_);
    bar_ = new QProgressBar;
    bar_->setObjectName("pybmProgressBar");
    bar_->setTextVisible(false);
    progressColumn->addWidget(bar_);
    detail_ = note("pybmProgressDetail");
    progressColumn->addWidget(detail_);
    progressColumn->addStretch(1);
    auto progressButtons = new QHBoxLayout;
    progressButtons->addStretch();
    stop_ = new QPushButton(tr("Stop"));
    stop_->setObjectName("pybmStopButton");
    progressButtons->addWidget(stop_);
    progressColumn->addLayout(progressButtons);
    pages_->addWidget(progress);

    // Third page: what became of everything.
    auto result = new QWidget;
    auto resultColumn = new QVBoxLayout(result);
    resultColumn->setContentsMargins(0, 0, 0, 0);
    resultColumn->setSpacing(10);
    resultTitle_ = new QLabel;
    resultTitle_->setObjectName("pybmResultTitle");
    auto titleFont = resultTitle_->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    resultTitle_->setFont(titleFont);
    resultColumn->addWidget(resultTitle_);
    resultText_ = new QLabel;
    resultText_->setObjectName("pybmResultText");
    resultText_->setWordWrap(true);
    resultText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultColumn->addWidget(resultText_);
    warning_ = new QLabel;
    warning_->setObjectName("pybmWarning");
    warning_->setWordWrap(true);
    warning_->setTextFormat(Qt::PlainText); // a folder's name is not markup
    warning_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // An amber tint, light enough for either theme, with the text in the theme's own colour.
    warning_->setStyleSheet("padding:8px 10px;border:1px solid rgba(217,142,4,150);"
                            "border-radius:6px;background:rgba(217,142,4,36);");
    warning_->hide();
    resultColumn->addWidget(warning_);
    resultColumn->addStretch(1);
    auto resultButtons = new QHBoxLayout;
    copy_ = new QPushButton(tr("Copy details"));
    copy_->setObjectName("pybmCopyButton");
    close_ = new QPushButton(tr("Close"));
    close_->setObjectName("pybmCloseButton");
    {
        // Bold like the other dialogs' main button; it becomes the default on its own page.
        auto font = close_->font();
        font.setWeight(QFont::DemiBold);
        close_->setFont(font);
    }
    resultButtons->addWidget(copy_);
    resultButtons->addStretch();
    resultButtons->addWidget(close_);
    resultColumn->addLayout(resultButtons);
    pages_->addWidget(result);

    connect(folder_, &QComboBox::activated, this, &PyBitmessageImportDialog::chooseFolder);
    connect(checkAgain_, &QPushButton::clicked, this, [this] { showFolder(shownFolder_); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(import_, &QPushButton::clicked, this, &PyBitmessageImportDialog::start);
    connect(stop_, &QPushButton::clicked, this, &PyBitmessageImportDialog::stop);
    connect(copy_, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(details()); });
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    connect(trash_, &QCheckBox::toggled, this, &PyBitmessageImportDialog::updateTrashHint);
    for (auto check : {identities_, contacts_, subscriptions_, inbox_, sent_, trash_})
        connect(check, &QCheckBox::toggled, this, [this] {
            import_->setEnabled((identities_->isChecked() && identities_->isEnabled()) ||
                                (contacts_->isChecked() && contacts_->isEnabled()) ||
                                (subscriptions_->isChecked() && subscriptions_->isEnabled()) ||
                                (inbox_->isChecked() && inbox_->isEnabled()) ||
                                (sent_->isChecked() && sent_->isEnabled()) ||
                                (trash_->isChecked() && trash_->isEnabled()));
        });
    if (folder_->count() > 1)
        showFolder(folder_->itemData(0).toString());
    else
        showFolder(QString());
    showPage(Choose);
}

PyBitmessageImportDialog::~PyBitmessageImportDialog() {
    if (importer_ && !finished_) {
        importer_->cancel(); // what is stored stays, whole chunks of it
        session_.setImporting(false);
    }
}

QString PyBitmessageImportDialog::timeLeftText(double seconds) {
    if (seconds < 0)
        return tr("estimating the time left…");
    if (seconds < 45)
        return tr("less than a minute left");
    const int minutes = int(std::lround(seconds / 60));
    if (minutes < 90)
        return tr("about %1 min left").arg(std::max(1, minutes));
    const int rounded = ((minutes + 2) / 5) * 5; // an hour or more is told in steps of five minutes
    return tr("about %1 h %2 min left").arg(rounded / 60).arg(rounded % 60);
}

void PyBitmessageImportDialog::setPaceForTesting(int chunk, int pauseMs) {
    chunk_ = chunk;
    budgetMs_ = 0;
    pauseMs_ = pauseMs;
}

// Two pages have a button of their own that Enter presses; a dialog has one default at a time.
void PyBitmessageImportDialog::showPage(Page page) {
    pages_->setCurrentIndex(page);
    // Off and on again: Qt tells the dialog of a default button only when the flag changes, and a
    // button put into a page before the page was in the dialog has told nobody.
    QPushButton *main = page == Choose ? import_ : page == Result ? close_ : nullptr;
    for (auto button : {import_, close_})
        button->setDefault(false);
    if (main)
        main->setDefault(true);
    adjustSize();
}

void PyBitmessageImportDialog::chooseFolder(int index) {
    const auto path = folder_->itemData(index).toString();
    if (!path.isEmpty()) {
        showFolder(path);
        return;
    }
    const auto start = shownFolder_.isEmpty() ? QDir::homePath() : shownFolder_;
    const auto chosen =
        QFileDialog::getExistingDirectory(this, tr("Choose the PyBitmessage data folder"), start);
    if (chosen.isEmpty()) { // back to the one shown before
        folder_->setCurrentIndex(std::max(0, folder_->findData(shownFolder_)));
        return;
    }
    int at = folder_->findData(chosen);
    if (at < 0) {
        at = folder_->count() - 1;
        folder_->insertItem(at, QDir::toNativeSeparators(chosen), chosen);
    }
    folder_->setCurrentIndex(at);
    showFolder(chosen);
}

void PyBitmessageImportDialog::showFolder(const QString &folder) {
    shownFolder_ = folder;
    const bool none = folder.isEmpty();
    if (none) {
        preview_ = bmc::pybm::Preview();
        preview_.trouble = Trouble::NothingThere;
    } else {
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        preview_ = bmc::pybm::preview(bridge::toStd(folder), &session_.coreVault());
        QGuiApplication::restoreOverrideCursor();
    }
    const bool problem = preview_.trouble != Trouble::None;
    // The trouble in words, and a way to look again when waiting may help.
    QString message;
    if (problem)
        message = none ? tr("No PyBitmessage data found on this computer. Choose its folder.")
                       : troubleText(preview_.trouble);
    if (problem && preview_.trouble != Trouble::NothingThere && !preview_.detail.empty())
        message += "\n" + tr("Details: %1").arg(bridge::fromStd(preview_.detail));
    trouble_->setText(message);
    trouble_->setVisible(problem);
    checkAgain_->setVisible(problem && !none && preview_.trouble != Trouble::NothingThere);

    const auto &p = preview_;
    const qint64 mail = p.inbox + p.channelLetters + p.broadcastLetters;
    const qint64 mailUnread = p.inboxUnread + p.channelUnread + p.broadcastUnread;
    const auto set = [&](QCheckBox *check, QLabel *count, qint64 n, const QString &text) {
        check->setEnabled(!problem && n > 0);
        check->setChecked(!problem && n > 0);
        check->setVisible(!problem);
        count->setVisible(!problem);
        count->setText(n > 0 ? text : QString::fromUtf8("—"));
    };
    set(identities_, identitiesCount_, p.keysEncrypted ? 0 : p.identities,
        tr("%1, chans: %2").arg(number(p.identities)).arg(number(p.chans)));
    set(contacts_, contactsCount_, p.contacts, number(p.contacts));
    set(subscriptions_, subscriptionsCount_, p.subscriptions, number(p.subscriptions));
    set(inbox_, inboxCount_, mail, tr("%1, unread: %2").arg(number(mail)).arg(number(mailUnread)));
    set(sent_, sentCount_, p.sent + p.unsent,
        tr("%1, not sent: %2").arg(number(p.sent + p.unsent)).arg(number(p.unsent)));
    set(trash_, trashCount_, p.trashed + p.sentTrashed, number(p.trashed + p.sentTrashed));

    QStringList lines;
    if (!problem) {
        if (p.running)
            lines << tr("PyBitmessage seems to be running. You can import now, but closing it "
                        "first gives a complete copy.");
        if (p.keysEncrypted)
            lines << tr("The keys in keys.dat are encrypted, so identities cannot be imported.");
        else if (p.keysUnreadable)
            lines << tr("keys.dat cannot be read, so identities cannot be imported.");
        if (p.disabled + p.subscriptionsDisabled > 0)
            lines << tr("Switched off in PyBitmessage, active here after the import: %1.")
                         .arg(number(p.disabled + p.subscriptionsDisabled));
        if (mail + p.sent + p.unsent + p.trashed + p.sentTrashed > 0)
            lines << tr("Letters keep their dates and whether they were read. Letters PyBitmessage "
                        "has not sent yet become drafts here; nothing is sent.");
        lines << tr("Not imported: network data, saved public keys, blacklist and whitelist.");
    }
    notes_->setText(lines.join("\n"));
    notes_->setVisible(!lines.isEmpty());
    import_->setEnabled(!problem && (identities_->isChecked() || contacts_->isChecked() ||
                                     subscriptions_->isChecked() || inbox_->isChecked() ||
                                     sent_->isChecked() || trash_->isChecked()));
    updateTrashHint();
    adjustSize();
}

// Deleted letters left out are not forgotten by the network: a node that still holds one gives it
// to the scan again as a new letter.
void PyBitmessageImportDialog::updateTrashHint() {
    trashHint_->setVisible(trash_->isEnabled() && !trash_->isChecked());
}

void PyBitmessageImportDialog::start() {
    bmc::pybm::Options options;
    options.folder = bridge::toStd(shownFolder_);
    options.identities = identities_->isChecked() && identities_->isEnabled();
    options.contacts = contacts_->isChecked() && contacts_->isEnabled();
    options.subscriptions = subscriptions_->isChecked() && subscriptions_->isEnabled();
    options.inbox = inbox_->isChecked() && inbox_->isEnabled();
    options.sent = sent_->isChecked() && sent_->isEnabled();
    options.trash = trash_->isChecked() && trash_->isEnabled();
    if (chunk_ > 0)
        options.chunk = chunk_;
    importer_ = std::make_unique<bmc::pybm::Importer>(options, session_.coreMailbox(),
                                                      session_.coreVault());
    session_.setImporting(true);
    finished_ = stopping_ = false;
    estimator_ = bmc::pybm::Estimator();
    clock_.start();
    phase_->setText(tr("Opening PyBitmessage's data…"));
    bar_->setRange(0, 0);
    detail_->clear();
    stop_->setEnabled(true);
    stop_->setText(tr("Stop"));
    showPage(Progress);
    timer_->start(0);
}

void PyBitmessageImportDialog::pump() {
    if (!importer_ || finished_)
        return;
    const bool more = importer_->step(budgetMs_);
    const auto &progress = importer_->progress();
    estimator_.sample(double(clock_.elapsed()) / 1000.0, double(progress.done),
                      double(progress.total));
    QString text;
    if (stopping_) {
        text = tr("Stopping…");
    } else if (progress.waiting) {
        text = tr("Waiting for PyBitmessage to release its data…");
    } else {
        switch (progress.phase) {
        case Phase::Contacts:
            text = tr("Importing the address book…");
            break;
        case Phase::Subscriptions:
            text = tr("Importing subscriptions…");
            break;
        case Phase::Checking:
            text = tr("Looking at your mailbox…");
            break;
        case Phase::Inbox:
            text = tr("Importing letters…");
            break;
        case Phase::Sent:
            text = tr("Importing sent letters…");
            break;
        case Phase::Identities:
            text = tr("Adding identities…");
            break;
        default:
            text = tr("Opening PyBitmessage's data…");
        }
    }
    phase_->setText(text);
    const bool letters = progress.phase == Phase::Inbox || progress.phase == Phase::Sent;
    if (letters && progress.total > 0 && !progress.waiting) {
        bar_->setRange(0, 1000);
        bar_->setValue(int(std::min<qint64>(1000, progress.done * 1000 / progress.total)));
        detail_->setText(tr("%1 of %2 letters · %3")
                             .arg(number(std::min(progress.done, progress.total)))
                             .arg(number(progress.total))
                             .arg(timeLeftText(estimator_.secondsLeft())));
    } else {
        bar_->setRange(0, 0); // busy, with no count to show
        detail_->clear();
    }
    if (more)
        timer_->start(pauseMs_);
    else
        finish();
}

void PyBitmessageImportDialog::stop() {
    if (!importer_ || finished_)
        return;
    importer_->cancel();
    stopping_ = true;
    stop_->setEnabled(false);
    stop_->setText(tr("Stopping…"));
}

void PyBitmessageImportDialog::reject() {
    if (running()) {
        stop();
        return;
    }
    QDialog::reject();
}

void PyBitmessageImportDialog::finish() {
    finished_ = true;
    timer_->stop();
    session_.setImporting(false);
    const auto &r = importer_->report();
    QStringList lines;
    const auto line = [&](const QString &format, qint64 n) {
        if (n > 0)
            lines << format.arg(number(n));
    };
    QString title = tr("Import finished");
    if (r.cancelled)
        title = tr("Import stopped");
    else if (!r.finished)
        title = tr("Import failed");
    resultTitle_->setText(title);
    qint64 added = 0, present = 0, unusable = 0, identitiesAdded = 0, identitiesPresent = 0;
    for (const auto &c : {r.inbox, r.channels, r.broadcasts, r.sent, r.drafts, r.trash}) {
        added += c.added;
        present += c.present;
    }
    for (const auto &i : r.identities) {
        using Result = bmc::IdentityImport::Result;
        identitiesAdded += i.result == Result::Added;
        identitiesPresent += i.result == Result::AlreadyThere;
        unusable += i.result == Result::Unusable;
    }
    if (added + present > 0)
        lines << tr("Letters added: %1").arg(number(added));
    line(tr("Letters already here: %1"), present);
    line(tr("Chat messages added: %1"), r.chats.added);
    line(tr("Chat messages already here: %1"), r.chats.present);
    line(tr("Deleted chat messages left out: %1"), r.chats.skipped);
    line(tr("Identities and chans added: %1"), identitiesAdded);
    line(tr("Identities already in the vault: %1"), identitiesPresent);
    line(tr("Contacts added: %1"), r.contacts.added);
    line(tr("Subscriptions added: %1"), r.subscriptions.added);
    line(tr("Unsent letters kept as drafts: %1"), r.drafts.added);
    if (unusable > 0) {
        lines << tr("Identities that could not be used: %1").arg(number(unusable));
        for (const auto &i : r.identities)
            if (i.result == bmc::IdentityImport::Result::Unusable)
                lines << QString::fromUtf8("   • ") +
                             (i.label.empty() ? bridge::fromStd(i.address)
                                              : bridge::fromStd(i.label)) +
                             " — " + reasonText(i.why);
    }
    if (r.keysEncrypted)
        lines << tr("The keys in keys.dat are encrypted, so identities cannot be imported.");
    else if (r.keysUnreadable)
        lines << tr("keys.dat cannot be read, so identities cannot be imported.");
    if (r.cancelled) {
        lines << QString()
              << tr("You stopped the import. What was imported stays; run it again to "
                    "carry on.");
    } else if (!r.finished) {
        lines << QString() << tr("The import could not be finished.");
        lines << troubleText(r.trouble);
        if (!r.detail.empty() && r.trouble != Trouble::NothingThere)
            lines << tr("Details: %1").arg(bridge::fromStd(r.detail));
    } else if (identitiesAdded > 0) {
        lines << QString()
              << tr("Your node now looks for letters to the new identities in the "
                    "objects it holds; they may take a moment to appear.");
    }
    resultText_->setText(lines.join("\n"));
    // All of it came over: the mailbox has dealt with PyBitmessage's data, and what is left of
    // PyBitmessage, its folder with everything in it unencrypted, is for the user to deal with.
    warning_->setVisible(r.finished);
    if (r.finished) {
        warning_->setText(tr("PyBitmessage keeps its data without encryption, your private keys "
                             "included. If you have fully moved to ynotbit, consider deleting the "
                             "folder %1. ynotbit never deletes it for you.")
                              .arg(QDir::toNativeSeparators(shownFolder_)));
        session_.settlePyBitmessageImport();
    }
    showPage(Result);
}

// The report for someone who has to be told about it: English, with all the counts.
QString PyBitmessageImportDialog::details() const {
    QStringList out;
    out << "PyBitmessage import";
    out << "Folder: " + shownFolder_;
    if (!importer_)
        return out.join("\n");
    const auto &r = importer_->report();
    const auto count = [](const bmc::pybm::Count &c) {
        return QString("added %1, already here %2, left out %3")
            .arg(c.added)
            .arg(c.present)
            .arg(c.skipped);
    };
    out << QString("Finished: %1, stopped by the user: %2, trouble: %3 %4")
               .arg(r.finished ? "yes" : "no")
               .arg(r.cancelled ? "yes" : "no")
               .arg(int(r.trouble))
               .arg(bridge::fromStd(r.detail));
    out << "Contacts: " + count(r.contacts);
    out << "Subscriptions: " + count(r.subscriptions);
    out << "Inbox: " + count(r.inbox);
    out << "Channels: " + count(r.channels);
    out << "Broadcasts: " + count(r.broadcasts);
    out << "Sent: " + count(r.sent);
    out << "Drafts: " + count(r.drafts);
    out << "Trash: " + count(r.trash);
    out << "Chats: " + count(r.chats);
    out << QString("Letters left out by the choices: %1, in a folder PyBitmessage does not list: "
                   "%2, without a date: %3")
               .arg(r.left)
               .arg(r.letterOfUnknownFolder)
               .arg(r.withoutDate);
    out << QString("keys.dat encrypted: %1, unreadable: %2")
               .arg(r.keysEncrypted ? "yes" : "no")
               .arg(r.keysUnreadable ? "yes" : "no");
    for (const auto &i : r.identities)
        out << QString("Identity %1 (%2)%3%4: result %5, reason %6")
                   .arg(bridge::fromStd(i.address), bridge::fromStd(i.label), i.chan ? " chan" : "",
                        i.wasDisabled ? " was switched off" : "")
                   .arg(int(i.result))
                   .arg(int(i.why));
    return out.join("\n");
}
} // namespace bm
