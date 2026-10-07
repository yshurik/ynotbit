#include "settings_window.h"
#include "appearance.h"
#include "session.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
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
SettingsWindow::SettingsWindow(Session &session, QWidget *parent)
    : QWidget(parent, Qt::Window), session_(session) {
    setObjectName("settingsWindow");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Settings"));
    resize(680, 460);
    auto layout = new QHBoxLayout(this);
    sections_ = new QListWidget;
    sections_->setObjectName("settingsSections");
    sections_->setFixedWidth(170);
    pages_ = new QStackedWidget;
    layout->addWidget(sections_);
    layout->addWidget(pages_, 1);
    connect(sections_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    addPage("network", tr("Network"), networkPage());
    addPage("storage", tr("Storage"), storagePage());
    sections_->setCurrentRow(0);
    connect(&session_, &Session::changed, this, &SettingsWindow::refresh);
    refresh();
}
void SettingsWindow::addPage(const QString &id, const QString &title, QWidget *page) {
    auto item = new QListWidgetItem(title, sections_);
    item->setData(Qt::UserRole, id);
    auto top = new QWidget; // pages keep their controls at the top
    auto column = new QVBoxLayout(top);
    column->addWidget(page);
    column->addStretch(1);
    pages_->addWidget(top);
}
void SettingsWindow::showPage(const QString &id) {
    for (int row = 0; row < sections_->count(); ++row)
        if (sections_->item(row)->data(Qt::UserRole).toString() == id)
            sections_->setCurrentRow(row);
}
QWidget *SettingsWindow::networkPage() {
    auto page = new QWidget;
    auto column = new QVBoxLayout(page);
    networkEnabled_ = new QCheckBox(tr("Network enabled"));
    networkEnabled_->setObjectName("networkEnabledCheck");
    connect(networkEnabled_, &QCheckBox::toggled, &session_, &Session::setNetworkEnabled);
    column->addWidget(networkEnabled_);
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
QWidget *SettingsWindow::storagePage() {
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
void SettingsWindow::refresh() {
    {
        QSignalBlocker block(networkEnabled_);
        networkEnabled_->setChecked(session_.networkEnabled());
    }
    usage_->setText(tr("%1 objects, %2 MB")
                        .arg(session_.objectCount())
                        .arg(session_.cacheBytes() / 1048576.0, 0, 'f', 1));
}
} // namespace bm
