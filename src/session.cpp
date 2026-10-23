#include "session.h"
#include "appearance.h"
#include "chat_format.h"
#include "coolname.h"
#include "i18n.h"
#include "message_model.h"
#include "pow.h"
#include "protocol.h"
#include "protocol_wire.h"
#include "qt_bridge.h"
#include "scanner.h"
#include "updates.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <bmc/clock.h>
#include <bmc/path.h>
#include <stdexcept>
namespace bm {
static QString addressInput(const QString &title, const QString &label, bool *accepted) {
    QInputDialog dialog;
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setInputMode(QInputDialog::TextInput);
    if (auto field = dialog.findChild<QLineEdit *>())
        field->setFont(addressFont());
    *accepted = dialog.exec() == QDialog::Accepted;
    return dialog.textValue();
}
// Messages arrive already translated (tr() at each call site).
static void check(bool b, const QString &m) {
    if (!b)
        throw std::runtime_error(m.toStdString());
}
struct Password {
    QByteArray bytes;
    ~Password() {
        if (!bytes.isEmpty())
            sodium_memzero(bytes.data(), bytes.size());
    }
};
static Password password(const QString &title, bool confirm = false) {
    bool ok = false;
    QString text = QInputDialog::getText(nullptr, title, Session::tr("Vault password"),
                                         QLineEdit::Password, {}, &ok);
    if (!ok)
        throw std::runtime_error("Cancelled");
    Password p{text.toUtf8()};
    text.fill(QChar(0));
    check(!p.bytes.isEmpty(), Session::tr("Password cannot be empty"));
    if (confirm) {
        auto repeated = QInputDialog::getText(nullptr, title, Session::tr("Repeat password"),
                                              QLineEdit::Password, {}, &ok);
        auto b = repeated.toUtf8();
        bool same = ok && b == p.bytes;
        sodium_memzero(b.data(), b.size());
        repeated.fill(QChar(0));
        check(same, Session::tr("Passwords do not match"));
    }
    return p;
}
static QString chooseSave(const QString &title, const QString &filter, const QString &suffix) {
    auto p = QFileDialog::getSaveFileName(
        nullptr, title, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        filter);
    if (!p.isEmpty() && !p.endsWith(suffix))
        p += suffix;
    return p;
}
static QString documentsPath() {
    const auto path = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return path.isEmpty() ? QDir::homePath() : path;
}
// The mailbox setting that says the Sent folder is in the address book (addSentRecipients).
static const QString kSentRecipientsSetting = "migrated:sent-recipients";
Session::Session(QString root, bool offline, QObject *parent)
    : QObject(parent), root_(std::move(root)), offline_(offline) {
    messageModel_ = std::make_unique<MessageModel>(this, this);
    QDir().mkpath(root_);
    nodeLock_ = std::make_unique<QLockFile>(root_ + "/desktop.lock");
    check(nodeLock_->tryLock(), tr("Another app instance is using this node folder"));
    client_ = std::make_shared<bmc::Client>(vaultCore_, mailboxCore_, bridge::toStd(root_));
    QSettings recent(root_ + "/desktop.ini", QSettings::IniFormat);
    vaultPath_ = recent.value("vault").toString();
    mailPath_ = recent.value("mailbox").toString();
    recentVaultPaths_ = recent.value("recentVaults").toStringList();
    recentMailboxPaths_ = recent.value("recentMailboxes").toStringList();
    retentionMB_ = std::clamp(recent.value("retentionMB", 2048).toInt(), 64, 32768);
    retentionDays_ = std::clamp(recent.value("retentionDays", 90).toInt(), 1, 3650);
    listenPort_ = configuredPort();
    restartSoon_.setSingleShot(true);
    restartSoon_.setInterval(1000);
    connect(&restartSoon_, &QTimer::timeout, this, &Session::restartNode);
    ProofOfWork::setGpuEnabled(QSettings().value("gpu", true).toBool());
    portMapper_ = std::make_unique<PortMapper>(miniupnpcBackend());
    connect(portMapper_.get(), &PortMapper::changed, this, &Session::changed);
    node_ = std::make_unique<bmc::NodeSupervisor>(nodeConfig());
    startNode();
    connect(&timer_, &QTimer::timeout, this, &Session::tick);
    timer_.start(750);
    activity_ = tr("Waiting for network objects");
}
Session::~Session() {
    timer_.stop();
    lock();
    // Idempotent, and a no-op when nothing runs; the node also stops on its own when the app is
    // gone (the relay bridge checks the parent). Even so, a killed node would not flush its store.
    if (node_)
        node_->stop();
}
QString Session::status() const {
    if (offline_)
        return tr("Offline · outgoing objects stay queued");
    if (!node_->running())
        return nodeError_.isEmpty()
                   ? tr("Node stopped · outgoing objects stay queued")
                   : tr("Node stopped: %1 · outgoing objects stay queued").arg(nodeError_);
    // Windows refuses the open for the instant the relay atomically replaces
    // the file; keep the last status read rather than flickering to "starting".
    if (const auto read =
            bmc::NodeStatus::fromFile(bmc::path(bridge::toStd(root_ + "/status.json"))))
        lastNodeStatus_ = read;
    else if (!lastNodeStatus_)
        return tr("Node starting · connecting to peers");
    const auto &status = *lastNodeStatus_;
    if (bmc::now() - status.time > 10)
        return tr("Node status unavailable");
    auto peers = status.peers;
    auto pending = status.pending;
    // "label: n" wording, so no language needs plural forms here.
    auto base = peers ? tr("Connected peers: %1 · receiving and relaying").arg(peers)
                      : tr("No connected peers · waiting for network");
    if (pending > 0)
        base += " · " + tr("objects downloading: %1").arg(pending);
    const int port = status.listenPort;
    if (port > 0 && port != configuredPort())
        base += " · " + tr("port %1 is taken, listening on %2").arg(configuredPort()).arg(port);
    return base;
}
// A service may ask for a port of its own (listenPort in its settings); a desktop listens on the
// protocol's.
quint16 Session::configuredPort() const {
    return quint16(nodeConfig().listenPort());
}
// The settings the node is started with, in the core's terms: the app's own binary, started with
// --node, so the node is this program and its behaviour is the one the desktop has always had.
bmc::NodeConfig Session::nodeConfig() const {
    QSettings config(root_ + "/desktop.ini", QSettings::IniFormat);
    bmc::NodeConfig node;
    node.root = bridge::toStd(root_);
    node.maildir = bridge::toStd(root_ + "/unused-maildir");
    node.bin = QCoreApplication::applicationFilePath().toStdString();
    node.listen = listenEnabled() && proxy().isEmpty();
    node.retentionMB = retentionMB_;
    node.retentionDays = retentionDays_;
    node.maxOutgoing = config.value("maxOutgoing").toInt();
    node.maxIncoming = config.value("maxIncoming").toInt();
    node.port = config.value("listenPort").toInt();
    node.peer = bridge::toStd(config.value("peer").toString());
    node.proxy = bridge::toStd(proxy());
    return node;
}
// Lower layers throw English (see src/i18n/error_catalog.cpp); messages this
// class raised itself are already translated and pass through unchanged.
QString Session::errorText(const std::exception &e) {
    return QCoreApplication::translate("bm::Errors", e.what());
}
QString Session::document() const {
    return mailboxOpen() ? QFileInfo(mailPath_).fileName() : tr("No mailbox open");
}
QVariantList Session::identities() const {
    QVariantList result;
    for (const auto &i : vault_.identities())
        result << QVariantMap{
            {"label", i.label}, {"address", i.address}, {"chan", i.chan}, {"default", i.isDefault}};
    return result;
}
void Session::attempt(const std::function<void()> &f) {
    if (busy_)
        return;
    busy_ = true;
    try {
        error_.clear();
        f();
    } catch (const std::exception &e) {
        auto message = errorText(e);
        if (message != "Cancelled")
            error_ = message;
    }
    busy_ = false;
    emit changed();
}
void Session::acquireVault(const QString &p) {
    vaultLock_ = lockVaultFile(p);
}
std::unique_ptr<QLockFile> Session::lockVaultFile(const QString &p) {
    auto lock = std::make_unique<QLockFile>(p + ".lock");
    if (!lock->tryLock()) {
        // Only a lock held elsewhere is another instance; else the place is the problem.
        check(lock->error() != QLockFile::LockFailedError,
              tr("Vault is in use by another instance"));
        check(false, tr("Cannot write %1").arg(QDir::toNativeSeparators(p)));
    }
    return lock;
}
void Session::rememberVault(const QString &path) {
    recentVaultPaths_.removeAll(path);
    recentVaultPaths_.prepend(path);
    while (recentVaultPaths_.size() > 8)
        recentVaultPaths_.removeLast();
}
void Session::rememberMailbox(const QString &path) {
    recentMailboxPaths_.removeAll(path);
    recentMailboxPaths_.prepend(path);
    while (recentMailboxPaths_.size() > 8)
        recentMailboxPaths_.removeLast();
}
QVariantList Session::recentVaults() const {
    QVariantList result;
    for (const auto &p : recentVaultPaths_)
        if (QFileInfo::exists(p))
            result << QVariantMap{{"name", QFileInfo(p).fileName()}, {"path", p}};
    return result;
}
QVariantList Session::recentMailboxes() const {
    QVariantList result;
    for (const auto &p : recentMailboxPaths_)
        if (QFileInfo::exists(p))
            result << QVariantMap{{"name", QFileInfo(p).fileName()}, {"path", p}};
    return result;
}
bool Session::createVault(const QString &path, QString password, QString repeated,
                          const Identity &identity) {
    attempt([&] {
        check(!password.isEmpty(), tr("Password cannot be empty"));
        check(password == repeated, tr("Passwords do not match"));
        check(!QFile::exists(path),
              tr("Choose a new filename; existing vaults are never overwritten"));
        Password secret{password.toUtf8()};
        password.fill(QChar(0));
        repeated.fill(QChar(0));
        // The new file reserved first: until then the open vault stays open.
        auto reserved = lockVaultFile(path);
        if (unlocked())
            lock();
        vaultLock_ = std::move(reserved);
        try {
            vault_.create(path, secret.bytes); // leaves whatever was at path alone
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
        try {
            vault_.addIdentity(identity);
        } catch (...) {
            vault_.lock();
            QFile::remove(path); // made just above, without its identity
            vaultLock_.reset();
            throw;
        }
        vaultPath_ = path;
        mailPath_.clear();
        mailKey_.clear();
        rememberVault(path);
        activity_ = tr("Vault created with %1. Create a mailbox to start.").arg(identity.label);
    });
    return error_.isEmpty();
}
void Session::openVault() {
    attempt([&] {
        check(!unlocked(), tr("Lock the current vault first"));
        auto p = QFileDialog::getOpenFileName(nullptr, tr("Open vault"), documentsPath(),
                                              tr("Bitmessage vault (*.bmvault)"));
        if (p.isEmpty())
            return;
        auto pass = password(tr("Unlock vault"));
        acquireVault(p);
        try {
            vault_.unlock(p, pass.bytes);
            vaultPath_ = p;
            mailPath_.clear();
            mailKey_.clear();
            rememberVault(p);
            activity_ = tr("Vault unlocked. Open a mailbox to inspect cached objects.");
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
    });
}
void Session::unlockVault() {
    if (vaultPath_.isEmpty()) {
        openVault();
        return;
    }
    attempt([&] {
        if (unlocked())
            return;
        auto pass = password(tr("Unlock vault"));
        acquireVault(vaultPath_);
        try {
            vault_.unlock(vaultPath_, pass.bytes);
            rememberVault(vaultPath_);
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
        if (!mailPath_.isEmpty())
            openMailboxPath(mailPath_);
    });
}
void Session::askForVault(const QString &path) {
    if (unlocked()) {
        if (path == vaultPath_)
            return; // it is the open one
        lock();
    }
    pendingVaultPath_ = path;
    emit vaultPasswordRequired(path);
}
void Session::beginVaultOpen() {
    attempt([&] {
        // Locking, if there is a vault to lock, is after the file is chosen: Cancel keeps it.
        auto p = QFileDialog::getOpenFileName(nullptr, tr("Open vault"), documentsPath(),
                                              tr("Bitmessage vault (*.bmvault)"));
        if (!p.isEmpty())
            askForVault(p);
    });
}
void Session::beginVaultOpenAt(QString path) {
    attempt([&] { askForVault(path); });
}
void Session::beginVaultUnlock() {
    if (vaultPath_.isEmpty()) {
        beginVaultOpen();
        return;
    }
    attempt([&] {
        check(!unlocked(), tr("Vault is already unlocked"));
        pendingVaultPath_ = vaultPath_;
        emit vaultPasswordRequired(vaultPath_);
    });
}
void Session::submitVaultPassword(QString passphrase) {
    attempt([&] {
        check(!pendingVaultPath_.isEmpty(), tr("Choose a vault first"));
        check(!passphrase.isEmpty(), tr("Password cannot be empty"));
        const auto path = pendingVaultPath_;
        Password secret{passphrase.toUtf8()};
        auto &bytes = secret.bytes;
        passphrase.fill(QChar(0));
        acquireVault(path);
        try {
            vault_.unlock(path, bytes);
            if (vaultPath_ != path) {
                mailPath_.clear();
                mailKey_.clear();
            }
            vaultPath_ = path;
            rememberVault(path);
            activity_ = tr("Vault unlocked. Open a mailbox to inspect cached objects.");
        } catch (...) {
            vaultLock_.reset();
            sodium_memzero(bytes.data(), bytes.size());
            throw;
        }
        sodium_memzero(bytes.data(), bytes.size());
        pendingVaultPath_.clear();
        emit vaultPasswordAccepted();
        if (!mailPath_.isEmpty())
            openMailboxPath(mailPath_);
        emit changed();
    });
}
void Session::createMailbox() {
    emit aboutToCloseMailbox();
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));

        auto p = chooseSave(tr("Create mailbox"), tr("Bitmessage mailbox (*.bmmail)"), ".bmmail");
        if (p.isEmpty())
            return;
        check(!QFile::exists(p),
              tr("Choose a new filename; existing mailbox documents are never overwritten"));
        client_->delivery().stop();
        mailbox_.close();
        clearMessages();
        mailLock_.reset();
        mailLock_ = std::make_unique<QLockFile>(p + ".lock");
        check(mailLock_->tryLock(), tr("Mailbox is in use"));
        try {
            auto id = vault_.addMailboxKey();
            mailbox_.create(p, id, vault_.mailboxKey(id));
            mailbox_.setSetting(kSentRecipientsSetting, "1"); // nothing was sent yet to add
            // A new mailbox starts with the built-in contacts, not at its next opening.
            try {
                seedContacts();
            } catch (...) {
                // Not added is no reason to refuse the mailbox.
            }
            mailPath_ = p;
            mailKey_ = id;
            rememberMailbox(p);
            bindMailboxToCache();
            refresh();
        } catch (...) {
            mailLock_.reset();
            throw;
        }
    });
}
void Session::bindMailboxToCache() {
    client_->bindMailboxToCache();
}
void Session::openMailboxPath(const QString &p) {
    client_->delivery().stop();
    check(unlocked(), tr("Unlock a vault first"));
    mailbox_.close();
    clearMessages();
    mailLock_.reset();
    mailLock_ = std::make_unique<QLockFile>(p + ".lock");
    check(mailLock_->tryLock(), tr("Mailbox is open in another instance"));
    for (const auto &id : vault_.mailboxIds()) {
        try {
            mailbox_.open(p, vault_.mailboxKey(id));
            check(mailbox_.keyId() == id, tr("Mailbox key identifier mismatch"));
            bindMailboxToCache();
            try {
                seedSubscriptions();
            } catch (...) {
                // A default subscription not added is no reason to refuse the mailbox.
            }
            // The built-in contacts first, so a letter once sent to one does not name it.
            try {
                seedContacts();
            } catch (...) {
                // Nor is a built-in contact not added; the next opening tries again.
            }
            try {
                addSentRecipients();
            } catch (...) {
                // Nor are the people written to; the next opening tries again.
            }
            // ynotbit opens on the first built-in subscription: the digest.
            if (const auto defaults = updates::defaultSubscriptions(); !defaults.isEmpty())
                welcomeSource_ = defaults.first().address;
            mailPath_ = p;
            mailKey_ = id;
            rememberMailbox(p);
            refresh();
            return;
        } catch (...) {
            mailbox_.close();
        }
    }
    mailLock_.reset();
    throw std::runtime_error(
        tr("This vault cannot open that mailbox, or the mailbox is damaged").toStdString());
}
void Session::openMailbox() {
    emit aboutToCloseMailbox();
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        auto p = QFileDialog::getOpenFileName(nullptr, tr("Open mailbox"), documentsPath(),
                                              tr("Bitmessage mailbox (*.bmmail)"));
        if (!p.isEmpty())
            openMailboxPath(p);
    });
}
void Session::openMailboxAt(QString path) {
    emit aboutToCloseMailbox();
    attempt([&] { openMailboxPath(path); });
}
void Session::lock() {
    emit aboutToCloseMailbox();
    client_->delivery().stop();
    QSettings recent(root_ + "/desktop.ini", QSettings::IniFormat);
    recent.setValue("vault", vaultPath_);
    recent.setValue("mailbox", mailPath_);
    recent.setValue("recentVaults", recentVaultPaths_);
    recent.setValue("recentMailboxes", recentMailboxPaths_);
    mailbox_.close();
    clearMessages();
    vault_.lock();
    mailLock_.reset();
    vaultLock_.reset();
    activity_ = tr("Vault locked. Objects remain cached for later inspection.");
    emit locked();
    emit changed();
}
bool Session::addIdentity(const Identity &identity) {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        vault_.addIdentity(identity);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = tr("Identity created. Retained objects will be inspected again.");
    });
    return error_.isEmpty();
}
bool Session::joinChannel(QString phrase, const QString &name, const QString &expected) {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        vault_.addChannel(phrase, name, expected);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = tr("Chan joined. The address appears in Identities.");
    });
    phrase.fill(QChar(0));
    return error_.isEmpty();
}
void Session::importIdentities() {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        auto p =
            QFileDialog::getOpenFileName(nullptr, tr("Import notbit / PyBitmessage identities"),
                                         documentsPath(), tr("Key files (*.dat);;All files (*)"));
        if (p.isEmpty())
            return;
        vault_.importKeys(p);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ =
            tr("Identities imported into the encrypted vault. The source file was preserved.");
    });
}
void Session::changePassword() {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        auto pass = password(tr("Change vault password"), true);
        vault_.changePassword(pass.bytes);
    });
}
void Session::backup() {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        auto dir =
            QFileDialog::getExistingDirectory(nullptr, tr("Choose backup folder"), documentsPath());
        if (dir.isEmpty())
            return;
        auto base =
            dir + "/bitmessage-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss");
        mailbox_.backup(base + ".bmmail", vault_.mailboxKey(mailKey_));
        check(QFile::copy(vaultPath_, base + ".bmvault"),
              tr("Mailbox backed up, but vault copy failed"));
        activity_ = tr("Mailbox and vault backup saved to %1").arg(dir);
    });
}
void Session::saveDraft(QString recipient, QString subject, QString body) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        check(body.toUtf8().size() <= kMaxDraftBytes, tr("Draft is too large"));
        mailbox_.saveDraft({}, {}, recipient, subject, body);
        refresh();
        activity_ = tr("Draft saved in the encrypted mailbox. It has not been sent.");
    });
}
void Session::rescan() {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.advance(0);
    });
}
void Session::refresh() {
    checkArrivals();
    if (!mailboxOpen()) {
        clearMessages();
        return;
    }
    if (displayedRevision_ == mailbox_.messageRevision())
        return;
    displayedRevision_ = mailbox_.messageRevision();
    emit messagesChanged();
    if (messageModel_)
        messageModel_->reload();
}
QVariantList Session::channels() const {
    QMap<QString, QString> labels;
    if (mailboxOpen())
        for (const auto &address : mailbox_.channelAddresses())
            labels[address] = address;
    if (unlocked())
        for (const auto &i : vault_.identities())
            if (i.chan)
                labels[i.address] = i.label.isEmpty() ? i.address : i.label;
    QVariantList result;
    for (auto i = labels.cbegin(); i != labels.cend(); ++i)
        result << QVariantMap{{"address", i.key()}, {"label", i.value()}};
    return result;
}
// One row of the letter list.
static QVariantMap listRow(const Mailbox &mailbox, const Message &m,
                           const QHash<QString, QString> &names) {
    OutboxItem out;
    if (m.folder == "Outbox" || m.folder == "Sent") {
        try {
            out = mailbox.outgoing(m.hash);
        } catch (...) {
        }
    }
    return QVariantMap{
        {"hash", m.hash},
        {"from", m.from},
        {"to", m.to},
        {"fromName", names.value(m.from) == m.from ? QString() : names.value(m.from)},
        {"toName", names.value(m.to) == m.to ? QString() : names.value(m.to)},
        {"subject", m.subject},
        {"preview", m.body},
        {"folder", m.folder},
        {"state", out.state},
        {"deliveryError", out.error},
        {"unread", mailbox.unread(m.hash)},
        {"kind", out.kind.isEmpty() ? mailbox.setting("draftkind:" + m.hash, "direct") : out.kind},
        {"received", formatDateTime(QDateTime::fromSecsSinceEpoch(m.received))}};
}
QVariantList Session::messagePage(const QString &folder, const QString &search, int offset,
                                  int limit, const QString &recipient, bool unreadOnly,
                                  bool anonymousOnly) const {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    const auto names = this->names();
    for (const auto &m : mailbox_.messageSummaries(folder, search, offset, limit, recipient,
                                                   unreadOnly, anonymousOnly))
        result << listRow(mailbox_, m, names);
    return result;
}
QHash<QString, QVariantMap> Session::messageRows(const QStringList &hashes) const {
    QHash<QString, QVariantMap> result;
    if (!mailboxOpen())
        return result;
    const auto names = this->names();
    for (const auto &m : mailbox_.messageSummaries(hashes))
        result.insert(m.hash, listRow(mailbox_, m, names));
    return result;
}
QVariantMap Session::message(QString id) const {
    if (!mailboxOpen())
        return {};
    const auto m = mailbox_.message(id);
    OutboxItem out;
    try {
        out = mailbox_.outgoing(id);
    } catch (...) {
    }
    return {
        {"hash", m.hash},
        {"from", m.from},
        {"to", m.to},
        {"subject", m.subject},
        {"body", m.body},
        {"preview", m.body.left(240)},
        {"storedAt", m.received},
        {"folder", m.folder},
        {"state", out.state},
        {"deliveryError", out.error},
        {"unread", mailbox_.unread(m.hash)},
        {"kind", out.kind.isEmpty() ? mailbox_.setting("draftkind:" + m.hash, "direct") : out.kind},
        {"received", formatDateTime(QDateTime::fromSecsSinceEpoch(m.received))}};
}
void Session::checkArrivals() {
    if (!mailboxOpen()) {
        arrivalsPrimed_ = false;
        return;
    }
    const quint64 letters = mailbox_.incomingLetterCount();
    const quint64 chats = mailbox_.incomingChatCount();
    // The first look at a freshly opened mailbox is the starting point: what an
    // existing mailbox already holds is not an arrival.
    if (!arrivalsPrimed_) {
        arrivalsPrimed_ = true;
        seenLetters_ = letters;
        seenChats_ = chats;
        return;
    }
    const bool letter = letters > seenLetters_;
    const bool chat = chats > seenChats_;
    seenLetters_ = letters;
    seenChats_ = chats;
    if (letter)
        emit incomingLetter();
    if (chat)
        emit incomingChat();
}
void Session::clearMessages() {
    displayedRevision_.reset();
    if (messageModel_)
        messageModel_->reload();
    emit messagesChanged();
}
void Session::tick() {
    if (busy_)
        return;
    try {
        pollNode();
        // The node rewrites status.json every second; offline, its last values stand.
        const auto read =
            bmc::NodeStatus::fromFile(bmc::path(bridge::toStd(root_ + "/status.json")));
        if (read) {
            lastNodeStatus_ = read;
            objectCount_ = read->objects;
            objectBytes_ = read->objectBytes;
            incoming_ = read->establishedIncoming;
            // The node listens elsewhere while another program holds its port,
            // and moves back when the port frees: the router mapping follows.
            if (read->listenPort > 0 && read->listenPort != listenPort_) {
                listenPort_ = quint16(read->listenPort);
                updatePortMapping();
            }
        }
        // A node that runs but does no good (its status stopped, or no peer for too long) is
        // started again; a node that has exited is restarted by pollNode().
        if (!offline_ && node_->running()) {
            using Verdict = bmc::NodeWatch::Verdict;
            const auto now = bmc::now();
            const auto verdict = nodeWatch_.judge(now, lastNodeStatus_ ? lastNodeStatus_->time : 0,
                                                  lastNodeStatus_ ? lastNodeStatus_->peers : 0);
            if (verdict != Verdict::Fine) {
                // For logs, not for the window: not translated.
                watchdogNote_ =
                    verdict == Verdict::Hung
                        ? QString("its status stopped for %1 s").arg(nodeWatch_.hungAfter)
                        : QString("no peer for %1 minutes").arg(nodeWatch_.lonelyAfter / 60);
                ++watchdogRestarts_;
                restartNode();
            }
        }
        if (mailboxOpen()) {
            const auto report = client_->step(bmc::now(), !offline_ && node_->running());
            if (report.open) {
                refresh();
                activity_ = report.upToDate ? tr("Mailbox up to date with retained objects")
                                            : tr("Inspecting cached objects · checkpoint %1")
                                                  .arg(qint64(report.checkpoint));
            }
            if (report.working)
                activity_ = tr("Preparing outgoing proof of work · locking pauses preparation");
            if (report.pruned)
                activity_ =
                    tr("Retention cleanup removed older objects · Some older letters may no "
                       "longer be recoverable");
        }
    } catch (const std::exception &e) {
        error_ = errorText(e);
    }
    emit changed();
}
QString Session::chatState(QString id) const {
    if (!mailboxOpen() || id.isEmpty())
        return {};
    try {
        return bridge::fromStd(client_->chatState(bridge::toStd(id)));
    } catch (const std::exception &) {
        return {};
    }
}
bool Session::sendChat(QString from, QString peer, QString body, QString *id) {
    bool queued = false;
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        const auto queuedId = client_->sendChat(bridge::toStd(from), bridge::toStd(peer),
                                                bridge::toStd(body), bmc::now());
        if (id)
            *id = bridge::fromStd(queuedId);
        refresh();
        queued = true;
    });
    return queued;
}
QVariantList Session::chatMessages(QString local, QString peer, int limit, bool *more) const {
    QVariantList result;
    if (more)
        *more = false;
    if (!mailboxOpen() || local.isEmpty() || peer.isEmpty())
        return result;
    for (const auto &message : mailbox_.chatMessages(local, peer, limit, more))
        result << QVariantMap{{"id", message.id},
                              {"body", message.body},
                              {"outgoing", message.outgoing},
                              {"seqno", message.seqno},
                              {"received", message.received},
                              {"expires", message.expires},
                              {"state", message.state}};
    return result;
}
// Not through attempt(): this runs from the window's refresh, and attempt() would
// refresh it again.
int Session::markChatRead(QString local, QString peer) {
    if (!mailboxOpen() || local.isEmpty() || peer.isEmpty())
        return 0;
    try {
        return mailbox_.markChatRead(local, peer);
    } catch (const std::exception &) {
        return 0;
    }
}
QVariantList Session::unreadChats(int limit) const {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    try {
        for (const auto &message : client_->unreadChats(limit))
            result << QVariantMap{{"id", bridge::fromStd(message.id)},
                                  {"local", bridge::fromStd(message.localAddress)},
                                  {"peer", bridge::fromStd(message.peerAddress)},
                                  {"body", bridge::fromStd(message.body)},
                                  // qint64, explicitly: QVariant's six integer constructors leave
                                  // the core's int64_t ambiguous where it is `long` (Linux), while
                                  // it is `long long` (macOS) and exact there.
                                  {"seqno", qint64(message.seqno)},
                                  {"received", qint64(message.received)}};
    } catch (const std::exception &) {
        result.clear();
    }
    return result;
}
// Not through attempt(), like markChatRead: a service calls it between polls.
bool Session::markChatMessageRead(QString id) {
    if (!mailboxOpen() || id.isEmpty())
        return false;
    try {
        return client_->markChatMessageRead(bridge::toStd(id));
    } catch (const std::exception &) {
        return false;
    }
}
// The chats of this vault's identities, the one with the newest message first. A chat is
// between one identity and a peer: one peer under two identities is two chats. Each has
// the peer's name, the identity's, its newest message and how many are unread.
QVariantList Session::chatConversations() const {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    QSet<QString> mine;
    for (const auto &identity : vault_.identities())
        if (!identity.chan)
            mine.insert(identity.address);
    const auto labels = names();
    for (const auto &summary : mailbox_.chatSummaries()) {
        if (!mine.contains(summary.local))
            continue;
        result << QVariantMap{{"key", chatKey(summary.local, summary.peer)},
                              {"address", summary.peer},
                              {"label", labels.value(summary.peer, summary.peer)},
                              {"local", summary.local},
                              {"localLabel", labels.value(summary.local, summary.local)},
                              {"body", summary.body},
                              {"received", summary.received},
                              {"outgoing", summary.outgoing},
                              {"state", summary.state},
                              {"unread", summary.unread}};
    }
    return result;
}
QString Session::saveLetter(QString id, QString from, QString to, QString subject, QString body,
                            QString kind) {
    QString result;
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        result = mailbox_.saveDraft(id, from, to.trimmed(), subject, body);
        mailbox_.setSetting("draftkind:" + result, kind == "broadcast" ? "broadcast" : "direct");
        refresh();
    });
    return result;
}
bool Session::sendLetter(QString id) {
    bool sent = false;
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        const auto m = mailbox_.message(id);
        // The one rule with UI wording of its own: the client's refusal is a bare backstop.
        const int size = letterTextBytes(m.subject, m.body);
        check(size <= kMaxLetterText,
              tr("This letter is %1; Bitmessage carries at most %2. Shorten it, or trim the quote.")
                  .arg(QLocale().formattedDataSize(size, 0, QLocale::DataSizeTraditionalFormat),
                       QLocale().formattedDataSize(kMaxLetterText, 0,
                                                   QLocale::DataSizeTraditionalFormat)));
        client_->sendLetter(bridge::toStd(id), bmc::now());
        refresh();
        sent = true;
        activity_ = tr("Letter queued. Follow its progress in Outbox.");
    });
    return sent;
}
void Session::retryLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        client_->delivery().retry(mailbox_.core(), bridge::toStd(id));
        refresh();
    });
}
void Session::cancelLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        client_->delivery().cancel(mailbox_.core(), bridge::toStd(id));
        refresh();
    });
}
void Session::moveLetter(QString id, QString folder) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.moveMessage(id, folder);
        refresh();
    });
}
void Session::restoreLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.restoreMessage(id);
        refresh();
    });
}
void Session::deleteLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        if (QMessageBox::question(
                nullptr, tr("Delete letter permanently?"),
                tr("This removes the letter from this mailbox. Backups are unchanged.")) !=
            QMessageBox::Yes)
            return;
        mailbox_.deleteMessage(id);
        refresh();
    });
}
void Session::readLetter(QString id) {
    attempt([&] {
        if (mailboxOpen()) {
            // Catch up first only if another operation changed the mailbox. Marking
            // one row read must not reload all message bodies from SQLCipher.
            refresh();
            mailbox_.markRead(id);
            displayedRevision_ = mailbox_.messageRevision();
            messageModel_->markRead(id);
            emit messageRead(id);
        }
    });
}
int Session::markLettersRead(QStringList ids) {
    int changed = 0;
    attempt([&] {
        if (!mailboxOpen())
            return;
        changed = mailbox_.markRead(ids);
        refresh(); // the revision moved: the list and the channel rail follow
    });
    return changed;
}
QVariantList Session::deliveryHistory(QString id) {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    try {
        for (const auto &e : mailbox_.events(id))
            result << QVariantMap{
                {"time", formatDateTime(QDateTime::fromSecsSinceEpoch(e.timestamp), true)},
                {"state", e.state},
                {"detail", e.detail}};
    } catch (const std::exception &e) {
        error_ = errorText(e);
    }
    return result;
}
QVariantList Session::subscriptions() const {
    QVariantList result;
    if (mailboxOpen())
        for (const auto &s : mailbox_.subscriptions())
            result << QVariantMap{{"address", s.address}, {"label", s.label}};
    return result;
}
void Session::seedSubscriptions() {
    for (const auto &d : updates::defaultSubscriptions()) {
        const auto key = updates::kSeededSetting + d.address;
        if (!Wire::validAddress(d.address) || !mailbox_.setting(key).isEmpty())
            continue;
        bool known = false;
        for (const auto &s : mailbox_.subscriptions())
            known = known || s.address == d.address;
        if (!known)
            mailbox_.subscribe(d.address, tr(d.label));
        mailbox_.setSetting(key, "1");
    }
}
void Session::seedContacts() {
    Mailbox::Batch batch(mailbox_); // one commit
    for (const auto &d : updates::defaultContacts()) {
        const auto key = updates::kSeededContactSetting + d.address;
        // Not once more, nor for an address that cannot be a contact (one of the vault's own).
        if (!contactProblem(d.address).isEmpty() || !mailbox_.setting(key).isEmpty())
            continue;
        if (!isContact(d.address))
            mailbox_.saveContact(d.address, d.label.isEmpty() ? d.address : d.label);
        if (!d.nickname.isEmpty())
            mailbox_.setSetting(updates::kNicknameSetting + d.address, d.nickname);
        mailbox_.setSetting(key, "1");
    }
    batch.commit();
}
void Session::addSentRecipients() {
    if (!mailbox_.setting(kSentRecipientsSetting).isEmpty())
        return;
    // Not for those already in the book, nor the vault's own identities and chans.
    QSet<QString> known;
    for (const auto &c : mailbox_.contacts())
        known << c.address;
    for (const auto &i : vault_.identities())
        known << i.address;
    Mailbox::Batch batch(mailbox_); // one commit, however many are added
    // Sent, newest first, a page at a time: a long folder is not held whole.
    constexpr int kPage = 500;
    for (int offset = 0;; offset += kPage) {
        const auto page = mailbox_.messageSummaries("Sent", {}, offset, kPage);
        for (const auto &letter : page) {
            const auto address = letter.to.trimmed();
            if (known.contains(address) || !Wire::validAddress(address))
                continue;
            known << address;
            const auto name = addressTitle(address);
            mailbox_.saveContact(address, name.isEmpty() ? address : name);
        }
        if (page.size() < kPage)
            break;
    }
    mailbox_.setSetting(kSentRecipientsSetting, "1");
    batch.commit();
}
QVariantList Session::broadcastSources() const {
    QMap<QString, QVariantMap> sources;
    if (!mailboxOpen())
        return {};
    const auto names = this->names();
    for (const auto &address : mailbox_.channelAddresses("Broadcasts"))
        sources[address] = {{"address", address},
                            {"label", names.value(address, address)},
                            {"subscribed", false},
                            {"updates", false}};
    if (updateNotices())
        sources[updates::publisherAddress()] = {{"address", updates::publisherAddress()},
                                                {"label", tr("ynotbit updates")},
                                                {"subscribed", true},
                                                {"updates", true}};
    for (const auto &s : mailbox_.subscriptions())
        sources[s.address] = {
            {"address", s.address},
            {"label", s.label.trimmed().isEmpty() ? s.address : s.label.trimmed()},
            {"subscribed", true},
            {"updates", s.address == updates::publisherAddress()}};
    QVariantList result;
    for (const auto &v : sources)
        result << v;
    std::stable_sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["label"].toString().compare(b.toMap()["label"].toString(),
                                                     Qt::CaseInsensitive) < 0;
    });
    return result;
}
QString Session::availableUpdate() const {
    if (!mailboxOpen() || !updateNotices())
        return {};
    const auto latest = mailbox_.setting(updates::kLatestSetting);
    if (latest.isEmpty() || latest == mailbox_.setting(updates::kDismissedSetting) ||
        updates::compareVersions(latest, QCoreApplication::applicationVersion()) <= 0)
        return {};
    return latest;
}
void Session::dismissUpdate() {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.setSetting(updates::kDismissedSetting, mailbox_.setting(updates::kLatestSetting));
    });
}
bool Session::updateNotices() const {
    return mailboxOpen() && !updates::publisherAddress().isEmpty() &&
           mailbox_.setting(updates::kNotifySetting) != "off";
}
void Session::setUpdateNotices(bool on) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.setSetting(updates::kNotifySetting, on ? "on" : "off");
    });
}
QVariantList Session::contacts() const {
    QVariantList result;
    if (mailboxOpen())
        for (const auto &c : mailbox_.contacts())
            result << QVariantMap{{"address", c.address},
                                  {"label", c.label},
                                  {"nickname", nickname(c.address)}};
    return result;
}
QString Session::nickname(QString address) const {
    return mailboxOpen() ? mailbox_.setting(updates::kNicknameSetting + address.trimmed())
                         : QString();
}
void Session::setNickname(QString address, QString nickname) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        address = address.trimmed();
        check(Wire::validAddress(address), tr("That is not a valid Bitmessage address"));
        mailbox_.setSetting(updates::kNicknameSetting + address, nickname.trimmed());
    });
}
QString Session::contactProblem(QString address) const {
    address = address.trimmed();
    if (!mailboxOpen())
        return tr("Open a mailbox first");
    if (address.isEmpty())
        return tr("Enter a BM- address");
    if (!Wire::validAddress(address))
        return tr("That is not a valid Bitmessage address");
    for (const auto &i : vault_.identities())
        if (i.address == address)
            return i.chan ? tr("That is one of your chans")
                          : tr("That is one of your own identities");
    return {};
}
bool Session::addContact(QString address, QString label) {
    attempt([&] {
        address = address.trimmed();
        const auto problem = contactProblem(address);
        if (!problem.isEmpty())
            throw std::runtime_error(problem.toStdString());
        label = label.trimmed();
        mailbox_.saveContact(address, label.isEmpty() ? address : label);
        messageModel()->reload(); // list rows carry correspondents' names
        activity_ = tr("Contact saved.");
    });
    return error_.isEmpty();
}
void Session::removeContact(QString address) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.removeContact(address);
        messageModel()->reload();
    });
}
bool Session::isContact(QString address) const {
    if (mailboxOpen())
        for (const auto &c : mailbox_.contacts())
            if (c.address == address)
                return true;
    return false;
}
QHash<QString, QString> Session::names() const {
    // Lowest precedence first, so later inserts win.
    QHash<QString, QString> result;
    if (!updates::publisherAddress().isEmpty())
        result[updates::publisherAddress()] = tr("ynotbit updates");
    if (mailboxOpen()) {
        for (const auto &s : mailbox_.subscriptions())
            if (!s.label.trimmed().isEmpty())
                result[s.address] = s.label.trimmed();
        for (const auto &c : mailbox_.contacts())
            result[c.address] = c.label;
    }
    if (unlocked())
        for (const auto &i : vault_.identities())
            if (!i.label.isEmpty())
                result[i.address] = i.label;
    return result;
}
QString Session::nameFor(QString address) const {
    const auto name = names().value(address);
    return name == address ? QString() : name;
}
void Session::subscribe() {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        bool ok = false;
        auto address =
            addressInput(tr("Subscribe to broadcasts"), tr("Publisher BM-address"), &ok).trimmed();
        if (!ok)
            return;
        check(Wire::validAddress(address), tr("Invalid Bitmessage address"));
        auto label = QInputDialog::getText(nullptr, tr("Subscription label"), tr("Label"),
                                           QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        mailbox_.subscribe(address, label);
        messageModel()->reload(); // a subscription's label names its letters
        activity_ = tr("Subscription saved. Retained broadcasts will be inspected.");
    });
}
void Session::unsubscribe(QString address) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        mailbox_.unsubscribe(address);
        messageModel()->reload();
    });
}
void Session::renameIdentity(QString address) {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        bool ok = false;
        auto label = QInputDialog::getText(nullptr, tr("Rename identity"), tr("Label"),
                                           QLineEdit::Normal, {}, &ok);
        if (ok)
            vault_.renameIdentity(address, label);
    });
}
void Session::copyAddress(QString address) {
    QApplication::clipboard()->setText(address);
}
void Session::setDefaultIdentity(QString address) {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        vault_.setDefaultIdentity(address);
    });
}
void Session::deleteIdentity(QString address) {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        if (QMessageBox::question(
                nullptr, tr("Delete identity permanently?"),
                tr("This permanently removes the private key for this address. Mail already sent "
                   "or received stays in your mailbox, but you will no longer be able to send as "
                   "this address or read anything newly sent to it.")) != QMessageBox::Yes)
            return;
        vault_.deleteIdentity(address);
    });
}
void Session::closeMailbox() {
    emit aboutToCloseMailbox();
    client_->delivery().stop();
    mailbox_.close();
    mailLock_.reset();
    clearMessages();
    mailPath_.clear();
    mailKey_.clear();
    emit changed();
}
QStringList Session::nodeArguments() const {
    QStringList args;
    for (const auto &argument : nodeConfig().nodeArguments())
        args << QString::fromStdString(argument);
    return args;
}
// What the node said, and when it went: what the three QProcess signals used to do here.
void Session::pollNode() {
    const auto line = QString::fromStdString(node_->standardError()).trimmed();
    if (!line.isEmpty() && line != lastStderrLine_) {
        lastStderrLine_ = line;
        nodeError_ = line;
        error_ = tr("Node: %1").arg(QString::fromStdString(node_->standardErrorChunk()));
        emit changed();
    }
    if (!node_->poll())
        return; // still running, or no exit since the last look
    if (nodeError_.isEmpty() && (node_->exitCode() || node_->crashed()))
        nodeError_ = node_->crashed() ? tr("crashed") : tr("exit code %1").arg(node_->exitCode());
    // What stopped it (a busy port, a locked store) may clear by itself.
    QTimer::singleShot(30000, this, [this] {
        if (!offline_ && !node_->running())
            startNode();
    });
    emit changed();
}
void Session::startNode() {
    updatePortMapping();
    if (offline_)
        return;
    nodeWatch_.restarted(bmc::now());
    QFile::remove(root_ + "/status.json");
    lastNodeStatus_.reset();
    lastStderrLine_.clear();
    nodeError_.clear();
    // The environment is the app's own plus YNOTBIT_PARENT_PID, which the node supervisor adds:
    // the relay bridge stops the node when this process is gone.
    if (!node_->start())
        error_ = tr("Node: %1").arg(QString::fromStdString(node_->lastError()));
}
void Session::restartNode() {
    ++nodeRestarts_;
    attempt([&] {
        node_->stop();
        startNode(); // which does the bookkeeping: a new start, not a new process only
    });
}
void Session::setNodeWatchdog(int lonelyMinutes) {
    nodeWatch_.lonelyAfter = std::max(0, lonelyMinutes) * 60;
}
void Session::setNetworkEnabled(bool enabled) {
    offline_ = !enabled;
    restartNode();
}
bool Session::validEndpoint(const QString &text) {
    return bmc::validEndpoint(text.toStdString());
}
QString Session::peer() const {
    return QSettings(root_ + "/desktop.ini", QSettings::IniFormat).value("peer").toString();
}
QString Session::proxy() const {
    return QSettings(root_ + "/desktop.ini", QSettings::IniFormat).value("proxy").toString();
}
// Saves one IP:port node setting; see setPeer().
static bool saveEndpoint(const QString &root, const char *key, const QString &value,
                         bool *changed) {
    if (!Session::validEndpoint(value))
        return false;
    QSettings config(root + "/desktop.ini", QSettings::IniFormat);
    *changed = config.value(key).toString() != value;
    if (*changed)
        config.setValue(key, value);
    return true;
}
bool Session::setPeer(QString value) {
    bool changed = false;
    if (!saveEndpoint(root_, "peer", value.trimmed(), &changed))
        return false;
    if (changed) {
        scheduleRestart();
        emit this->changed();
    }
    return true;
}
bool Session::setProxy(QString value) {
    bool changed = false;
    if (!saveEndpoint(root_, "proxy", value.trimmed(), &changed))
        return false;
    if (changed) {
        scheduleRestart();
        emit this->changed();
    }
    return true;
}
void Session::setRetention(int mb, int days) {
    mb = std::clamp(mb, 64, 32768);
    days = std::clamp(days, 1, 3650);
    if (mb == retentionMB_ && days == retentionDays_)
        return;
    retentionMB_ = mb;
    retentionDays_ = days;
    QSettings config(root_ + "/desktop.ini", QSettings::IniFormat);
    config.setValue("retentionMB", mb);
    config.setValue("retentionDays", days);
    scheduleRestart();
    emit changed();
}
void Session::scheduleRestart() {
    restartSoon_.start();
}
bool Session::gpuEnabled() const {
    return QSettings().value("gpu", true).toBool();
}
void Session::setGpuEnabled(bool on) {
    QSettings().setValue("gpu", on);
    ProofOfWork::setGpuEnabled(on);
    emit changed();
}
bool Session::listenEnabled() const {
    return QSettings(root_ + "/desktop.ini", QSettings::IniFormat).value("listen", true).toBool();
}
void Session::setListenEnabled(bool on) {
    if (on == listenEnabled())
        return;
    QSettings(root_ + "/desktop.ini", QSettings::IniFormat).setValue("listen", on);
    scheduleRestart();
    emit changed();
}
bool Session::upnpEnabled() const {
    return QSettings(root_ + "/desktop.ini", QSettings::IniFormat).value("upnp", true).toBool();
}
void Session::setUpnpEnabled(bool on) {
    if (on == upnpEnabled())
        return;
    QSettings(root_ + "/desktop.ini", QSettings::IniFormat).setValue("upnp", on);
    updatePortMapping();
    emit changed();
}
void Session::updatePortMapping() {
    if (wantsPortMapping(!offline_, listenEnabled(), upnpEnabled(), proxy()))
        portMapper_->start(listenPort_);
    else
        portMapper_->stop();
}
} // namespace bm
