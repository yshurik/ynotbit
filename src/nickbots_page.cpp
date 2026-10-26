#include "nickbots_page.h"
#include "appearance.h"
#include "letter_render.h"
#include "nick_client.h"
#include "session.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <bmc/nickbot.h>
#include <bmc/wire.h>

namespace bm {
namespace {
QLabel *note(const QString &text, const QString &name = {}) {
    auto label = new QLabel(text);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setStyleSheet("color:palette(mid);");
    return label;
}
QLabel *heading(const QString &text) {
    auto label = new QLabel(text);
    auto font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}
} // namespace

NickbotsPage::NickbotsPage(Session &session, QWidget *parent) : QWidget(parent), session_(session) {
    setObjectName("nickbotsPage");
    auto column = new QVBoxLayout(this);
    column->addWidget(
        note(tr("A nickbot gives people nicknames you can search for and add to your "
                "contacts. The first one is the default. Each has a colour, and a "
                "nickname is shown in the colour of the nickbot that vouches for it.")));
    enabled_ = new QCheckBox(tr("Use nickbots"));
    enabled_->setObjectName("nickbotsEnabledCheck");
    column->addWidget(enabled_);
    column->addWidget(
        note(tr("Nothing is asked of a nickbot unless you ask: Find people… and Check "
                "nicknames are on the Contacts page. A lookup is sent from a chan that "
                "everybody has, so the nickbot cannot tell who asks. It takes a few "
                "minutes. Turn nickbots off and nothing is sent at all.")));

    column->addWidget(heading(tr("Your nickbots")));
    bots_ = new QTreeWidget;
    bots_->setObjectName("nickbotsList");
    bots_->setColumnCount(2);
    bots_->setHeaderLabels({tr("Nickbot"), tr("Address")});
    bots_->setRootIsDecorated(false);
    bots_->setUniformRowHeights(true);
    bots_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bots_->setMinimumHeight(130);
    bots_->setMaximumHeight(170);
    bots_->header()->setStretchLastSection(true);
    bots_->setColumnWidth(0, 180);
    column->addWidget(bots_);
    auto buttons = new QHBoxLayout;
    const auto make = [&](const QString &text, const char *name) {
        auto b = new QPushButton(text);
        b->setObjectName(name);
        buttons->addWidget(b);
        return b;
    };
    add_ = make(tr("Add…"), "nickbotAddButton");
    up_ = make(tr("Move up"), "nickbotUpButton");
    down_ = make(tr("Move down"), "nickbotDownButton");
    remove_ = make(tr("Remove"), "nickbotRemoveButton");
    buttons->addStretch(1);
    column->addLayout(buttons);
    note_ = note({}, "nickbotsNote");
    column->addWidget(note_);
    column->addWidget(
        note(tr("To get a nickname for one of your identities, select it on the Identities page "
                "and choose Nickname…"),
             "nickbotsIdentitiesNote"));
    column->addStretch(1);

    connect(enabled_, &QCheckBox::toggled, this,
            [this](bool on) { session_.nicks().setEnabled(on); });
    connect(add_, &QPushButton::clicked, this, &NickbotsPage::addBot);
    connect(remove_, &QPushButton::clicked, this, &NickbotsPage::removeBot);
    connect(up_, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { move(1); });
    connect(bots_, &QTreeWidget::itemSelectionChanged, this, &NickbotsPage::refresh);
    connect(&session_.nicks(), &NickClient::changed, this, &NickbotsPage::refresh);
    connect(&session_, &Session::changed, this, &NickbotsPage::refresh);
    refresh();
}

void NickbotsPage::refresh() {
    auto &nicks = session_.nicks();
    const bool open = session_.mailboxOpen();
    {
        QSignalBlocker block(enabled_);
        enabled_->setChecked(nicks.enabled());
    }
    enabled_->setEnabled(open);
    // The nickbots, in order; the selection survives a refresh.
    const int row = bots_->currentItem() ? bots_->indexOfTopLevelItem(bots_->currentItem()) : -1;
    {
        QSignalBlocker block(bots_);
        bots_->clear();
        const auto list = nicks.bots();
        for (int i = 0; i < list.size(); ++i) {
            const auto name =
                list[i].label.isEmpty() ? list[i].address.left(14) + "…" : list[i].label;
            auto item = new QTreeWidgetItem(bots_);
            item->setText(0, i == 0 ? tr("%1 (default)").arg(name) : name);
            item->setIcon(0, dotIcon(nickbotColor(i)));
            item->setText(1, list[i].address);
            item->setFont(1, addressFont());
            item->setData(0, Qt::UserRole, list[i].address);
        }
        if (row >= 0 && row < bots_->topLevelItemCount())
            bots_->setCurrentItem(bots_->topLevelItem(row));
    }
    const int count = bots_->topLevelItemCount();
    const int at = bots_->currentItem() ? bots_->indexOfTopLevelItem(bots_->currentItem()) : -1;
    add_->setEnabled(open && count < bmc::nickbot::kMaxBots);
    remove_->setEnabled(open && at >= 0);
    up_->setEnabled(open && at > 0);
    down_->setEnabled(open && at >= 0 && at < count - 1);
    note_->setText(count == 0
                       ? tr("No nickbot is listed, so nothing can be looked up or registered.")
                   : count >= bmc::nickbot::kMaxBots ? tr("Five nickbots are listed, the most.")
                                                     : tr("Moving a nickbot moves its colour."));
}

void NickbotsPage::addBot() {
    QDialog dialog(this);
    dialog.setObjectName("nickbotAddDialog");
    dialog.setWindowTitle(tr("Add a nickbot"));
    auto form = new QFormLayout(&dialog);
    auto address = new QLineEdit;
    address->setObjectName("nickbotAddressField");
    address->setFont(addressFont());
    address->setPlaceholderText("BM-…");
    auto label = new QLineEdit;
    label->setObjectName("nickbotLabelField");
    label->setPlaceholderText(tr("Optional"));
    auto why = new QLabel;
    why->setObjectName("nickbotAddProblem");
    why->setStyleSheet("color:#d93025;");
    why->setWordWrap(true);
    form->addRow(tr("Address"), address);
    form->addRow(tr("Name"), label);
    form->addRow(why);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    auto ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(tr("Add"));
    ok->setEnabled(false);
    connect(address, &QLineEdit::textChanged, &dialog, [&] {
        ok->setEnabled(bmc::Wire::validAddress(address->text().trimmed().toStdString()));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto list = session_.nicks().bots();
        list.push_back({address->text().trimmed(), label->text().trimmed()});
        QString problem;
        if (session_.nicks().setBots(list, &problem))
            dialog.accept();
        else
            why->setText(problem);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void NickbotsPage::move(int by) {
    const int at = bots_->indexOfTopLevelItem(bots_->currentItem());
    auto list = session_.nicks().bots();
    const int to = at + by;
    if (at < 0 || to < 0 || to >= list.size())
        return;
    list.swapItemsAt(at, to);
    QString problem;
    session_.nicks().setBots(list, &problem);
    bots_->setCurrentItem(bots_->topLevelItem(to));
}

void NickbotsPage::removeBot() {
    const int at = bots_->indexOfTopLevelItem(bots_->currentItem());
    auto list = session_.nicks().bots();
    if (at < 0 || at >= list.size())
        return;
    list.removeAt(at);
    QString problem;
    session_.nicks().setBots(list, &problem);
}
} // namespace bm
