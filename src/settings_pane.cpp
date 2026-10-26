#include "settings_pane.h"
#include "app_badge.h"
#include "appearance.h"
#include "gpu_pow.h"
#include "i18n.h"
#include "nickbots_page.h"
#include "port_mapper.h"
#include "pow.h"
#include "session.h"
#include "sound_notifier.h"
#include "updates.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVBoxLayout>
#include <algorithm>
namespace bm {
namespace {
QLabel *heading(const QString &text) {
    auto label = new QLabel(text);
    auto font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}
QLabel *note(const QString &text, const QString &name = {}) {
    auto label = new QLabel(text);
    label->setObjectName(name);
    label->setWordWrap(true);
    return label;
}
} // namespace
SettingsPane::SettingsPane(Session &session, Appearance &appearance, QString density,
                           SoundNotifier &sounds, QWidget *parent)
    : QWidget(parent), session_(session), appearance_(appearance), sounds_(sounds) {
    setObjectName("settingsPane");
    // Headed like the other whole-pane pages, Identities and Contacts.
    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 0);
    outer->setSpacing(8);
    auto titleCol = new QVBoxLayout;
    auto heading = new QLabel(tr("Settings"));
    heading->setObjectName("settingsHeading");
    heading->setStyleSheet("font-size:20px;font-weight:600;");
    titleCol->addWidget(heading);
    auto sub = new QLabel(tr("Changes apply at once"));
    sub->setStyleSheet("color:palette(mid);font-size:12px;");
    titleCol->addWidget(sub);
    outer->addLayout(titleCol);
    auto layout = new QHBoxLayout;
    outer->addLayout(layout, 1);
    sections_ = new QListWidget;
    sections_->setObjectName("settingsSections");
    sections_->setFixedWidth(170);
    pages_ = new QStackedWidget;
    layout->addWidget(sections_);
    layout->addWidget(pages_, 1);
    connect(sections_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    addPage("network", tr("Network"), networkPage());
    addPage("storage", tr("Storage"), storagePage());
    addPage("sending", tr("Sending"), sendingPage());
    addPage("appearance", tr("Appearance"), appearancePage(density));
    addPage("notifications", tr("Notifications"), notificationsPage());
    addPage("nickbots", tr("Nickbots"), new NickbotsPage(session_));
    addPage("security", tr("Security"), securityPage());
    sections_->setCurrentRow(0);
    connect(&session_, &Session::changed, this, &SettingsPane::refresh);
    refresh();
}
void SettingsPane::addPage(const QString &id, const QString &title, QWidget *page) {
    auto item = new QListWidgetItem(title, sections_);
    item->setData(Qt::UserRole, id);
    auto top = new QWidget; // pages keep their controls at the top
    auto column = new QVBoxLayout(top);
    column->addWidget(page);
    column->addStretch(1);
    // A page taller than the window scrolls rather than squeezes its rows, or
    // holds the window taller.
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(top);
    pages_->addWidget(scroll);
}
void SettingsPane::showPage(const QString &id) {
    for (int row = 0; row < sections_->count(); ++row)
        if (sections_->item(row)->data(Qt::UserRole).toString() == id)
            sections_->setCurrentRow(row);
}
QWidget *SettingsPane::networkPage() {
    auto page = new QWidget;
    auto column = new QVBoxLayout(page);
    networkEnabled_ = new QCheckBox(tr("Network enabled"));
    networkEnabled_->setObjectName("networkEnabledCheck");
    connect(networkEnabled_, &QCheckBox::toggled, &session_, &Session::setNetworkEnabled);
    column->addWidget(networkEnabled_);
    column->addWidget(heading(tr("Incoming connections")));
    listen_ = new QCheckBox(tr("Accept incoming connections"));
    listen_->setObjectName("listenCheck");
    connect(listen_, &QCheckBox::toggled, &session_, &Session::setListenEnabled);
    upnp_ = new QCheckBox(tr("Open the port on the router (UPnP)"));
    upnp_->setObjectName("upnpCheck");
    connect(upnp_, &QCheckBox::toggled, &session_, &Session::setUpnpEnabled);
    proxyNote_ = note(tr("Off while a proxy is set: listening would reveal your real IP address."),
                      "proxyNote");
    incoming_ = note({}, "incomingStatus");
    column->addWidget(listen_);
    column->addWidget(upnp_);
    upnpStatus_ = note({}, "upnpStatus");
    column->addWidget(upnpStatus_);
    column->addWidget(proxyNote_);
    column->addWidget(incoming_);
    // An IP:port field: checked as it is typed, saved when editing ends.
    auto endpointField = [&](const QString &name, const QString &title, const QString &hint,
                             const QString &value, const QString &problem,
                             bool (Session::*save)(QString)) {
        column->addWidget(heading(title));
        auto edit = new QLineEdit(value);
        edit->setObjectName(name + "Edit");
        edit->setPlaceholderText(hint);
        edit->setFont(addressFont());
        auto error = note(problem, name + "Error");
        error->setStyleSheet("color: #d93025;");
        error->hide();
        connect(edit, &QLineEdit::textEdited, error, [error](const QString &text) {
            error->setVisible(!Session::validEndpoint(text.trimmed()));
        });
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, save] {
            if (Session::validEndpoint(edit->text().trimmed()))
                (session_.*save)(edit->text());
        });
        column->addWidget(edit);
        column->addWidget(error);
    };
    endpointField("proxy", tr("SOCKS5 proxy"),
                  tr("Empty for a direct connection; Tor is usually 127.0.0.1:9050"),
                  session_.proxy(), tr("Enter an IP address and port, e.g. 127.0.0.1:9050"),
                  &Session::setProxy);
    endpointField("peer", tr("Additional peer"), tr("Empty for automatic discovery"),
                  session_.peer(),
                  tr("Enter an IP address and port, e.g. 192.0.2.1:8444 or [::1]:8444"),
                  &Session::setPeer);
    auto restart = new QPushButton(tr("Restart node"));
    restart->setObjectName("restartNodeButton");
    connect(restart, &QPushButton::clicked, &session_, &Session::restartNode);
    column->addWidget(restart, 0, Qt::AlignLeft);
    return page;
}
QWidget *SettingsPane::storagePage() {
    auto page = new QWidget;
    auto form = new QFormLayout(page);
    auto mb = new QSpinBox;
    mb->setObjectName("retentionMBSpin");
    mb->setRange(64, 32768);
    mb->setSingleStep(64);
    mb->setSuffix(" MB");
    mb->setValue(session_.retentionMB());
    auto days = new QSpinBox;
    days->setObjectName("retentionDaysSpin");
    days->setRange(1, 3650);
    days->setValue(session_.retentionDays());
    auto save = [this, mb, days] { session_.setRetention(mb->value(), days->value()); };
    connect(mb, &QSpinBox::editingFinished, this, save);
    connect(days, &QSpinBox::editingFinished, this, save);
    form->addRow(tr("Keep at most"), mb);
    form->addRow(tr("Days to keep"), days);
    usage_ = new QLabel;
    usage_->setObjectName("storageUsage");
    form->addRow(tr("Now"), usage_);
    form->addRow(note(tr("Older objects are discarded first; letters saved in the mailbox are kept.")));
    auto rescan = new QPushButton(tr("Inspect retained objects again"));
    rescan->setObjectName("rescanButton");
    connect(rescan, &QPushButton::clicked, &session_, &Session::rescan);
    form->addRow(rescan);
    return page;
}
QWidget *SettingsPane::sendingPage() {
    auto page = new QWidget;
    auto column = new QVBoxLayout(page);
    auto gpu = new QCheckBox(tr("Proof of work on the GPU"));
    gpu->setObjectName("gpuCheck");
    gpu->setChecked(session_.gpuEnabled());
    connect(gpu, &QCheckBox::toggled, &session_, &Session::setGpuEnabled);
    auto device = note(tr("Looking for a GPU…"), "gpuDevice");
    // Opening the GPU compiles its kernel (about a second): not on the UI thread.
    QPointer<QLabel> label = device;
    QThreadPool::globalInstance()->start([label] {
        auto &solver = GpuSolver::instance();
        const QString text = solver.available() ? solver.api() + " · " + solver.deviceName()
                                                : solver.problem();
        QMetaObject::invokeMethod(qApp, [label, text] {
            if (label)
                label->setText(text);
        });
    });
    column->addWidget(gpu);
    column->addWidget(device);
    column->addWidget(note(tr("CPU workers: %1").arg(ProofOfWork::workerCount()), "cpuWorkers"));
    return page;
}
QWidget *SettingsPane::appearancePage(const QString &density) {
    auto page = new QWidget;
    auto form = new QFormLayout(page);
    auto theme = new QComboBox;
    theme->setObjectName("themeCombo");
    theme->addItem(tr("System"), QString("system"));
    theme->addItem(tr("Light"), QString("light"));
    theme->addItem(tr("Dark"), QString("dark"));
    theme->setCurrentIndex(std::max(0, theme->findData(appearance_.mode())));
    connect(theme, &QComboBox::currentIndexChanged, this,
            [this, theme] { appearance_.setMode(theme->currentData().toString()); });
    form->addRow(tr("Theme"), theme);
    auto language = new QComboBox;
    language->setObjectName("languageCombo");
    language->addItem(tr("System default"), QString());
    for (const auto &l : languages())
        language->addItem(l.nativeName, l.code);
    language->setCurrentIndex(std::max(0, language->findData(savedLanguage())));
    auto restartNote = note(tr("Restart ynotbit to use the new language."), "languageNote");
    restartNote->hide();
    connect(language, &QComboBox::currentIndexChanged, this, [language, restartNote] {
        saveLanguage(language->currentData().toString());
        restartNote->show();
    });
    form->addRow(tr("Language"), language);
    form->addRow(restartNote);
    density_ = new QComboBox;
    density_->setObjectName("densityCombo");
    density_->addItem(tr("Comfortable"), QString("comfortable"));
    density_->addItem(tr("Cozy"), QString("cozy"));
    density_->addItem(tr("Compact"), QString("compact"));
    density_->setCurrentIndex(std::max(0, density_->findData(density)));
    connect(density_, &QComboBox::currentIndexChanged, this,
            [this] { emit densityChosen(density_->currentData().toString()); });
    form->addRow(tr("Letter list"), density_);
    return page;
}
void SettingsPane::showDensity(const QString &density) {
    QSignalBlocker block(density_);
    density_->setCurrentIndex(std::max(0, density_->findData(density)));
}
QWidget *SettingsPane::notificationsPage() {
    auto page = new QWidget;
    auto column = new QVBoxLayout(page);
    updateNotices_ = new QCheckBox(tr("Notify about new ynotbit versions"));
    updateNotices_->setObjectName("updateNoticesCheck");
    connect(updateNotices_, &QCheckBox::toggled, &session_, &Session::setUpdateNotices);
    column->addWidget(updateNotices_);
    column->addWidget(note(tr("Kept in the open mailbox.")));
    column->addWidget(heading(tr("App icon")));
    badgeEnabled_ =
        new QCheckBox(tr("Show the number of unread letters and chats on the app icon"));
    badgeEnabled_->setObjectName("badgeEnabledCheck");
    badgeEnabled_->setChecked(AppBadge::enabled());
    connect(badgeEnabled_, &QCheckBox::toggled, this, [](bool on) { AppBadge::setEnabled(on); });
    column->addWidget(badgeEnabled_);
    column->addWidget(
        note(tr("In the Dock, the taskbar or the launcher, where the system shows one.")));
    column->addWidget(heading(tr("Sounds")));
    soundEnabled_ = new QCheckBox(tr("Play a sound for new messages"));
    soundEnabled_->setObjectName("soundEnabledCheck");
    soundEnabled_->setChecked(sounds_.enabled());
    connect(soundEnabled_, &QCheckBox::toggled, &sounds_, &SoundNotifier::setEnabled);
    column->addWidget(soundEnabled_);
    column->addWidget(note(tr("New letters and chats play the chosen theme's sound.")));
    soundThemes_ = new QListWidget;
    soundThemes_->setObjectName("soundThemesList");
    soundThemes_->setMaximumHeight(140);
    connect(soundThemes_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        const auto id = soundThemes_->item(row)->data(Qt::UserRole).toString();
        if (!id.isEmpty() && id != sounds_.theme())
            sounds_.setTheme(id);
    });
    column->addWidget(soundThemes_);
    addSoundTheme_ = new QPushButton(tr("Add theme from ZIP…"));
    addSoundTheme_->setObjectName("addSoundThemeButton");
    connect(addSoundTheme_, &QPushButton::clicked, this, &SettingsPane::addSoundTheme);
    column->addWidget(addSoundTheme_, 0, Qt::AlignLeft);
    column->addWidget(note(tr("A theme is a ZIP holding new-chat.wav and new-mail.wav, at its top "
                              "level or in one folder inside it. The sounds must be 16-bit PCM "
                              "WAV at a rate the output device plays.")));
    for (const auto &event : soundEvents()) {
        const QString label =
            event.id == "new-chat" ? tr("New chat message") : tr("New email");
        auto row = new QHBoxLayout;
        auto box = new QCheckBox(label);
        box->setObjectName("soundEvent_" + event.id);
        box->setChecked(sounds_.eventEnabled(event.id));
        connect(box, &QCheckBox::toggled, this,
                [this, id = event.id](bool on) { sounds_.setEventEnabled(id, on); });
        soundEvents_.insert(event.id, box);
        auto preview = new QPushButton(tr("Play"));
        preview->setObjectName("soundPreview_" + event.id);
        connect(preview, &QPushButton::clicked, &sounds_,
                [this, id = event.id] { sounds_.preview(id); });
        row->addWidget(box);
        row->addWidget(preview);
        row->addStretch(1);
        column->addLayout(row);
    }
    refreshSoundThemes();
    return page;
}
void SettingsPane::refreshSoundThemes() {
    if (!soundThemes_)
        return;
    QSignalBlocker block(soundThemes_);
    soundThemes_->clear();
    const auto themes = sounds_.themes();
    const QString chosen = sounds_.theme();
    QListWidgetItem *selected = nullptr;
    for (const auto &theme : themes) {
        auto *item = new QListWidgetItem(
            theme.builtin ? tr("%1 (built-in)").arg(theme.name) : theme.name, soundThemes_);
        item->setData(Qt::UserRole, theme.id);
        if (theme.id == chosen || (!selected && chosen.isEmpty() && theme.builtin))
            selected = item;
    }
    if (selected)
        soundThemes_->setCurrentItem(selected);
}
void SettingsPane::addSoundTheme() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add a sound theme"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("Sound themes (*.zip)"));
    if (path.isEmpty())
        return;
    SoundTheme theme;
    QString error;
    if (!importSoundTheme(path, sounds_.themesDir(), &theme, &error, sounds_.audioLimits())) {
        QMessageBox::warning(this, tr("Add a sound theme"), error);
        return;
    }
    sounds_.setTheme(theme.id);
    refreshSoundThemes();
}
QWidget *SettingsPane::securityPage() {
    auto page = new QWidget;
    auto column = new QVBoxLayout(page);
    changePassword_ = new QPushButton(tr("Change vault password…"));
    changePassword_->setObjectName("changePasswordButton");
    connect(changePassword_, &QPushButton::clicked, &session_, &Session::changePassword);
    column->addWidget(changePassword_, 0, Qt::AlignLeft);
    return page;
}
void SettingsPane::refresh() {
    {
        QSignalBlocker block(networkEnabled_);
        networkEnabled_->setChecked(session_.networkEnabled());
    }
    usage_->setText(tr("%1 objects, %2 MB")
                        .arg(session_.objectCount())
                        .arg(session_.cacheBytes() / 1048576.0, 0, 'f', 1));
    {
        QSignalBlocker block(updateNotices_);
        updateNotices_->setChecked(session_.updateNotices());
    }
    {
        QSignalBlocker block(soundEnabled_);
        soundEnabled_->setChecked(sounds_.enabled());
    }
    {
        QSignalBlocker block(badgeEnabled_);
        badgeEnabled_->setChecked(AppBadge::enabled());
    }
    updateNotices_->setEnabled(session_.mailboxOpen() && !updates::publisherAddress().isEmpty());
    changePassword_->setEnabled(session_.unlocked());
    const bool proxied = !session_.proxy().isEmpty();
    {
        QSignalBlocker a(listen_), b(upnp_);
        listen_->setChecked(session_.listenEnabled());
        upnp_->setChecked(session_.upnpEnabled());
    }
    listen_->setEnabled(!proxied);
    upnp_->setEnabled(!proxied && session_.listenEnabled());
    proxyNote_->setVisible(proxied);
    const int incoming = session_.incomingConnections();
    incoming_->setText(proxied || !session_.listenEnabled() ? QString()
                       : incoming > 0
                           ? tr("Reachable from outside ✓ (incoming connections: %1)").arg(incoming)
                           : tr("No incoming connections yet"));
    const auto &mapper = session_.portMapper();
    QString router;
    switch (mapper.state()) {
    case PortMapper::State::Off:
        break;
    case PortMapper::State::Searching:
        router = tr("Looking for the router…");
        break;
    case PortMapper::State::Mapped:
        router = tr("Port %1 is open on the router, external IP %2")
                     .arg(mapper.port())
                     .arg(mapper.externalIp());
        break;
    case PortMapper::State::NoGateway:
        router = tr("The router does not support UPnP; forward TCP %1 manually").arg(mapper.port());
        break;
    case PortMapper::State::CarrierNat:
        router = tr("The router's internet address %1 is shared by the provider (carrier-grade "
                    "NAT): incoming connections cannot reach it")
                     .arg(mapper.externalIp());
        break;
    case PortMapper::State::Failed:
        router = tr("The router refused the mapping: %1").arg(mapper.error());
        break;
    }
    upnpStatus_->setText(router);
    upnpStatus_->setVisible(!router.isEmpty());
}
} // namespace bm
