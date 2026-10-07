#include "session.h"
#include "i18n.h"
#include "appearance.h"
#include "message_model.h"
#include "pow.h"
#include "protocol.h"
#include "protocol_wire.h"
#include "scanner.h"
#include "updates.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace bm {
static constexpr quint16 kNodePort = 8444; // the node's NTB_PROTO_DEFAULT_PORT
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
    QString text =
        QInputDialog::getText(nullptr, title, Session::tr("Vault password"), QLineEdit::Password, {}, &ok);
    if (!ok)
        throw std::runtime_error("Cancelled");
    Password p{text.toUtf8()};
    text.fill(QChar(0));
    check(!p.bytes.isEmpty(), Session::tr("Password cannot be empty"));
    if (confirm) {
        auto repeated =
            QInputDialog::getText(nullptr, title, Session::tr("Repeat password"), QLineEdit::Password, {}, &ok);
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
Session::Session(QString root, bool offline, QObject *parent)
    : QObject(parent), root_(std::move(root)), offline_(offline) {
    messageModel_ = std::make_unique<MessageModel>(this, this);
    QDir().mkpath(root_);
    nodeLock_ = std::make_unique<QLockFile>(root_ + "/desktop.lock");
    check(nodeLock_->tryLock(), tr("Another app instance is using this node folder"));
    cache_ = std::make_unique<Cache>(root_);
    delivery_ = std::make_unique<Delivery>(root_);
    QSettings recent(root_ + "/desktop.ini", QSettings::IniFormat);
    vaultPath_ = recent.value("vault").toString();
    mailPath_ = recent.value("mailbox").toString();
    recentVaultPaths_ = recent.value("recentVaults").toStringList();
    recentMailboxPaths_ = recent.value("recentMailboxes").toStringList();
    retentionMB_ = std::clamp(recent.value("retentionMB", 2048).toInt(), 64, 32768);
    retentionDays_ = std::clamp(recent.value("retentionDays", 90).toInt(), 1, 3650);
    restartSoon_.setSingleShot(true);
    restartSoon_.setInterval(1000);
    connect(&restartSoon_, &QTimer::timeout, this, &Session::restartNode);
    ProofOfWork::setGpuEnabled(QSettings().value("gpu", true).toBool());
    connect(&node_, &QProcess::readyReadStandardOutput, this,
            [this] { node_.readAllStandardOutput(); });
    connect(&node_, &QProcess::readyReadStandardError, this, [this] {
        auto text = QString::fromUtf8(node_.readAllStandardError()).trimmed();
        if (!text.isEmpty()) {
            error_ = tr("Node: %1").arg(text.left(400));
            emit changed();
        }
    });
    connect(&node_, &QProcess::errorOccurred, this, [this] {
        error_ = tr("Node: %1").arg(node_.errorString());
        emit changed();
    });
    connect(&node_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) { emit changed(); });
    portMapper_ = std::make_unique<PortMapper>(miniupnpcBackend());
    connect(portMapper_.get(), &PortMapper::changed, this, &Session::changed);
    startNode();
    connect(&timer_, &QTimer::timeout, this, &Session::tick);
    timer_.start(750);
    activity_ = tr("Waiting for network objects");
}
Session::~Session() {
    timer_.stop();
    lock();
    if (node_.state() != QProcess::NotRunning) {
        node_.terminate();
        if (!node_.waitForFinished(3000)) {
            node_.kill();
            node_.waitForFinished(1000);
        }
    }
}
QString Session::status() const {
    if (offline_)
        return tr("Offline · outgoing objects stay queued");
    if (node_.state() != QProcess::Running)
        return tr("Node stopped · outgoing objects stay queued");
    QFile f(root_ + "/status.json");
    // Windows refuses the open for the instant the relay atomically replaces
    // the file; keep the last status read rather than flickering to "starting".
    if (f.open(QIODevice::ReadOnly))
        lastNodeStatus_ = QJsonDocument::fromJson(f.read(4096)).object();
    else if (lastNodeStatus_.isEmpty())
        return tr("Node starting · connecting to peers");
    const auto &status = lastNodeStatus_;
    if (QDateTime::currentSecsSinceEpoch() - status.value("time").toInteger() > 10)
        return tr("Node status unavailable");
    auto peers = status.value("peers").toInt();
    auto pending = status.value("pending").toInt();
    // "label: n" wording, so no language needs plural forms here.
    auto base = peers ? tr("Connected peers: %1 · receiving and relaying").arg(peers)
                      : tr("No connected peers · waiting for network");
    if (pending > 0)
        base += " · " + tr("objects downloading: %1").arg(pending);
    return base;
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
        result << QVariantMap{{"label", i.label},
                              {"address", i.address},
                              {"chan", i.chan},
                              {"default", i.isDefault}};
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
    vaultLock_ = std::make_unique<QLockFile>(p + ".lock");
    check(vaultLock_->tryLock(), tr("Vault is in use by another instance"));
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
void Session::createVault() {
    attempt([&] {
        check(!unlocked(), tr("Lock the current vault first"));
        auto p = chooseSave(tr("Create vault"), tr("Bitmessage vault (*.bmvault)"), ".bmvault");
        if (p.isEmpty())
            return;
        auto pass = password(tr("Create vault"), true);
        acquireVault(p);
        try {
            vault_.create(p, pass.bytes);
            vaultPath_ = p;
            mailPath_.clear();
            mailKey_.clear();
            rememberVault(p);
            activity_ = tr("Vault created. Add an identity and create a mailbox.");
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
    });
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
void Session::beginVaultCreate() {
    attempt([&] {
        check(!unlocked(), tr("Lock the current vault first"));
        auto p = chooseSave(tr("Create vault"), tr("Bitmessage vault (*.bmvault)"), ".bmvault");
        if (p.isEmpty())
            return;
        pendingVaultPath_ = p;
        emit vaultPasswordRequired(p, true);
    });
}
void Session::beginVaultOpen() {
    attempt([&] {
        check(!unlocked(), tr("Lock the current vault first"));
        auto p = QFileDialog::getOpenFileName(nullptr, tr("Open vault"), documentsPath(),
                                              tr("Bitmessage vault (*.bmvault)"));
        if (p.isEmpty())
            return;
        pendingVaultPath_ = p;
        emit vaultPasswordRequired(p, false);
    });
}
void Session::beginVaultUnlock() {
    if (vaultPath_.isEmpty()) {
        beginVaultOpen();
        return;
    }
    attempt([&] {
        check(!unlocked(), tr("Vault is already unlocked"));
        pendingVaultPath_ = vaultPath_;
        emit vaultPasswordRequired(vaultPath_, false);
    });
}
void Session::submitVaultPassword(QString passphrase, QString repeated, bool create) {
    attempt([&] {
        check(!pendingVaultPath_.isEmpty(), tr("Choose a vault first"));
        check(!passphrase.isEmpty(), tr("Password cannot be empty"));
        if (create)
            check(passphrase == repeated, tr("Passwords do not match"));
        const auto path = pendingVaultPath_;
        Password secret{passphrase.toUtf8()};
        auto &bytes = secret.bytes;
        passphrase.fill(QChar(0));
        repeated.fill(QChar(0));
        if (create) {
            check(!QFile::exists(path),
                  tr("Choose a new filename; existing vaults are never overwritten"));
            acquireVault(path);
            try {
                vault_.create(path, bytes);
                vaultPath_ = path;
                mailPath_.clear();
                mailKey_.clear();
                rememberVault(path);
                activity_ = tr("Vault created. Add an identity and create a mailbox.");
            } catch (...) {
                vaultLock_.reset();
                sodium_memzero(bytes.data(), bytes.size());
                throw;
            }
        } else {
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
        }
        sodium_memzero(bytes.data(), bytes.size());
        pendingVaultPath_.clear();
        emit vaultPasswordAccepted();
        if (!create && !mailPath_.isEmpty())
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
        delivery_->stop();
        mailbox_.close();
        clearMessages();
        mailLock_.reset();
        mailLock_ = std::make_unique<QLockFile>(p + ".lock");
        check(mailLock_->tryLock(), tr("Mailbox is in use"));
        try {
            auto id = vault_.addMailboxKey();
            mailbox_.create(p, id, vault_.mailboxKey(id));
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
    boundCacheId_ = cache_->id();
    mailbox_.bindCache(boundCacheId_);
}
void Session::openMailboxPath(const QString &p) {
    delivery_->stop();
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
    if (delivery_)
        delivery_->stop();
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
void Session::addIdentity() {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        bool ok;
        auto label =
            QInputDialog::getText(nullptr, tr("New identity"), tr("Label"), QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        vault_.addIdentity(label);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = tr("Identity created. Retained objects will be inspected again.");
    });
}
void Session::joinChannel() {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        bool ok;
        auto phrase =
            QInputDialog::getText(nullptr, tr("Join or create chan"), tr("Shared phrase (exact spelling)"),
                                  QLineEdit::Password, {}, &ok);
        if (!ok)
            return;
        auto expected =
            addressInput(tr("Verify chan address"),
                         tr("Expected BM-address (leave empty to create a version 4 chan)"), &ok);
        if (!ok) {
            phrase.fill(QChar(0));
            return;
        }
        vault_.addChannel(phrase, {}, expected.trimmed());
        phrase.fill(QChar(0));
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = tr("Chan joined. The address appears in Identities.");
    });
}
void Session::importIdentities() {
    attempt([&] {
        check(unlocked(), tr("Unlock a vault first"));
        auto p = QFileDialog::getOpenFileName(nullptr, tr("Import notbit / PyBitmessage identities"),
                                              documentsPath(), tr("Key files (*.dat);;All files (*)"));
        if (p.isEmpty())
            return;
        vault_.importKeys(p);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = tr("Identities imported into the encrypted vault. The source file was preserved.");
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
        // The node rewrites status.json every second; offline, its last values stand.
        QFile nodeStatus(root_ + "/status.json");
        if (nodeStatus.open(QIODevice::ReadOnly)) {
            const auto node = QJsonDocument::fromJson(nodeStatus.read(4096)).object();
            if (node.contains("objects")) {
                objectCount_ = node.value("objects").toInteger();
                objectBytes_ = node.value("object_bytes").toInteger();
                incoming_ = node.value("established_incoming").toInt();
            }
        }
        if (mailboxOpen()) {
            // A new id means the node created or emptied its store: read it from the start.
            if (cache_->id() != boundCacheId_)
                bindMailboxToCache();
            if (cache_->firstSequence() > mailbox_.checkpoint() + 1)
                prunedUnread_ = true;
            // A time budget, not a count: catching up (a new identity, first
            // sync) goes as fast as the window can spare, ~150 ms per tick.
            delivery_->scan(*cache_, mailbox_, vault_, 512, 150);
            delivery_->tick(mailbox_, vault_, !offline_ && node_.state() == QProcess::Running);
            refresh();
            activity_ = !cache_->hasAfter(mailbox_.checkpoint())
                            ? tr("Mailbox up to date with retained objects")
                            : tr("Inspecting cached objects · checkpoint %1")
                                  .arg(mailbox_.checkpoint());
        }
        if (delivery_->working())
            activity_ = tr("Preparing outgoing proof of work · locking pauses preparation");
        if (prunedUnread_)
            activity_ = tr("Retention cleanup removed older objects · Some older letters may no "
                           "longer be recoverable");
    } catch (const std::exception &e) {
        error_ = errorText(e);
    }
    emit changed();
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
        auto m = mailbox_.message(id);
        bool own = false;
        for (const auto &i : vault_.identities())
            if (i.address == m.from)
                own = true;
        check(own, tr("Choose a sender from this vault"));
        auto kind = mailbox_.setting("draftkind:" + id, "direct");
        check(kind == "broadcast" || Wire::validAddress(m.to),
              tr("Enter a valid Bitmessage recipient address"));
        check(!m.subject.contains('\n') && !m.subject.contains('\r'), tr("Subject must be one line"));
        check(!m.body.trimmed().isEmpty(), tr("Write a message before sending"));
        const int size = letterTextBytes(m.subject, m.body);
        check(size <= kMaxLetterText,
              tr("This letter is %1; Bitmessage carries at most %2. Shorten it, or trim the quote.")
                  .arg(QLocale().formattedDataSize(size, 0, QLocale::DataSizeTraditionalFormat),
                       QLocale().formattedDataSize(kMaxLetterText, 0,
                                                   QLocale::DataSizeTraditionalFormat)));
        mailbox_.queueDraft(id, kind, QDateTime::currentSecsSinceEpoch() + 4 * 86400);
        Delivery::rereadKept(mailbox_); // the recipient's key may already be kept
        refresh();
        sent = true;
        activity_ = tr("Letter queued. Follow its progress in Outbox.");
    });
    return sent;
}
void Session::retryLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        delivery_->retry(mailbox_, id);
        refresh();
    });
}
void Session::cancelLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), tr("Open a mailbox first"));
        delivery_->cancel(mailbox_, id);
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
QVariantList Session::deliveryHistory(QString id) {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    try {
        for (const auto &e : mailbox_.events(id))
            result << QVariantMap{
                {"time",
                 formatDateTime(QDateTime::fromSecsSinceEpoch(e.timestamp), true)},
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
        sources[s.address] = {{"address", s.address},
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
            result << QVariantMap{{"address", c.address}, {"label", c.label}};
    return result;
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
            return i.chan ? tr("That is one of your chans") : tr("That is one of your own identities");
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
        auto label =
            QInputDialog::getText(nullptr, tr("Rename identity"), tr("Label"), QLineEdit::Normal, {}, &ok);
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
    delivery_->stop();
    mailbox_.close();
    mailLock_.reset();
    clearMessages();
    mailPath_.clear();
    mailKey_.clear();
    emit changed();
}
QStringList Session::nodeArguments() const {
    QSettings config(root_ + "/desktop.ini", QSettings::IniFormat);
    auto peer = config.value("peer").toString(), proxy = config.value("proxy").toString();
    // Listening next to a proxy would publish the real IP the proxy hides.
    const bool listen = config.value("listen", true).toBool() && proxy.isEmpty();
    QStringList args{"--node", "-D", root_, "-m", root_ + "/unused-maildir"};
    if (!listen)
        args << "-i";
    args << "-R" << QString::number(retentionMB_) << "-A" << QString::number(retentionDays_);
    if (!peer.isEmpty())
        args << "-P" << peer << "-L";
    if (!proxy.isEmpty())
        args << "-r" << proxy << "-B";
    return args;
}
void Session::startNode() {
    updatePortMapping();
    if (offline_)
        return;
    QFile::remove(root_ + "/status.json");
    lastNodeStatus_ = {};
    node_.setProgram(QCoreApplication::applicationFilePath());
    node_.setArguments(nodeArguments());
    node_.start();
}
void Session::restartNode() {
    ++nodeRestarts_;
    attempt([&] {
        if (node_.state() != QProcess::NotRunning) {
            node_.terminate();
            if (!node_.waitForFinished(3000)) {
                node_.kill();
                node_.waitForFinished(1000);
            }
        }
        startNode();
    });
}
void Session::setNetworkEnabled(bool enabled) {
    offline_ = !enabled;
    restartNode();
}
bool Session::validEndpoint(const QString &text) {
    if (text.isEmpty())
        return true;
    QUrl url("tcp://" + text);
    QHostAddress address;
    return url.isValid() && url.userInfo().isEmpty() && url.path().isEmpty() &&
           url.query().isEmpty() && url.fragment().isEmpty() && address.setAddress(url.host()) &&
           url.port() > 0 && url.port() <= 65535;
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
        portMapper_->start(kNodePort);
    else
        portMapper_->stop();
}
} // namespace bm
