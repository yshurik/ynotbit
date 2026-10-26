#include "find_people_dialog.h"
#include "appearance.h"
#include "letter_document.h"
#include "nick_client.h"
#include "session.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <bmc/nickbot.h>
#include <bmc/wire.h>

namespace bm {
namespace {
QIcon dot(const QColor &color) {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(2, 2, 12, 12));
    return QIcon(pixmap);
}
namespace nb = bmc::nickbot;
QString q(const std::string &s) {
    return QString::fromStdString(s);
}
} // namespace

FindPeopleDialog::FindPeopleDialog(Session &session, QWidget *parent)
    : QDialog(parent), session_(session) {
    setObjectName("findPeopleDialog");
    setWindowTitle(tr("Find people"));
    resize(560, 420);
    auto column = new QVBoxLayout(this);
    auto intro =
        new QLabel(tr("Ask a nickbot. Type a nickname (@ben), a name to search for (at least 4 "
                      "letters) or an address (BM-…). The answer takes a few minutes: you can "
                      "close this window and come back."));
    intro->setWordWrap(true);
    intro->setStyleSheet("color:palette(mid);");
    column->addWidget(intro);
    auto form = new QFormLayout;
    bot_ = new QComboBox;
    bot_->setObjectName("findPeopleBot");
    const auto bots = session_.nicks().bots();
    for (int i = 0; i < bots.size(); ++i)
        bot_->addItem(dot(nickbotColor(i)),
                      bots[i].label.isEmpty() ? bots[i].address : bots[i].label, bots[i].address);
    form->addRow(tr("Nickbot"), bot_);
    auto row = new QHBoxLayout;
    field_ = new QLineEdit;
    field_->setObjectName("findPeopleField");
    field_->setPlaceholderText("@ben, ben1, BM-…");
    field_->setClearButtonEnabled(true);
    search_ = new QPushButton(tr("Search"));
    search_->setObjectName("findPeopleSearchButton");
    search_->setDefault(true);
    row->addWidget(field_, 1);
    row->addWidget(search_);
    form->addRow(tr("Find"), row);
    column->addLayout(form);
    status_ = new QLabel;
    status_->setObjectName("findPeopleStatus");
    status_->setWordWrap(true);
    column->addWidget(status_);
    results_ = new QTreeWidget;
    results_->setObjectName("findPeopleResults");
    results_->setColumnCount(3);
    results_->setHeaderLabels({tr("Nickname"), tr("Address"), tr("In contacts")});
    results_->setRootIsDecorated(false);
    results_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results_->setUniformRowHeights(true);
    results_->header()->setStretchLastSection(false);
    results_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    results_->setColumnWidth(0, 170);
    column->addWidget(results_, 1);
    auto buttons = new QHBoxLayout;
    add_ = new QPushButton(tr("Add to contacts"));
    add_->setObjectName("findPeopleAddButton");
    auto close = new QPushButton(tr("Close"));
    close->setObjectName("findPeopleCloseButton");
    buttons->addStretch(1);
    buttons->addWidget(add_);
    buttons->addWidget(close);
    column->addLayout(buttons);

    connect(search_, &QPushButton::clicked, this, &FindPeopleDialog::ask);
    connect(field_, &QLineEdit::returnPressed, this, &FindPeopleDialog::ask);
    connect(add_, &QPushButton::clicked, this, &FindPeopleDialog::addSelected);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(results_, &QTreeWidget::itemSelectionChanged, this, &FindPeopleDialog::updateButtons);
    connect(field_, &QLineEdit::textChanged, this, &FindPeopleDialog::updateButtons);
    connect(&session_.nicks(), &NickClient::lookupFinished, this, &FindPeopleDialog::answered);
    if (bots.isEmpty())
        status_->setText(tr("No nickbot is listed. Add one in Settings > Nickbots."));
    updateButtons();
}

void FindPeopleDialog::updateButtons() {
    search_->setEnabled(bot_->count() > 0 && !field_->text().trimmed().isEmpty());
    add_->setEnabled(!results_->selectedItems().isEmpty());
}

// What is typed says what to ask: an address gets its nicknames, a whole nickname its address, and
// any other word is searched for.
void FindPeopleDialog::ask() {
    const auto text = field_->text().trimmed();
    const auto bot = bot_->currentData().toString();
    QString problem;
    results_->clear();
    key_.clear();
    if (bmc::Wire::validAddress(text.toStdString())) {
        kind_ = "nickname";
        key_ = q(nb::keyOfNicknameRequest({text.toStdString()}));
        problem = session_.nicks().lookupNicknames({text}, bot, true);
    } else if (nb::parseNick(text.toStdString())) {
        kind_ = "address";
        key_ = q(nb::keyOfAddressRequest(*nb::parseNick(text.toStdString())));
        problem = session_.nicks().lookupAddress(text, bot);
    } else {
        kind_ = "search";
        nb::Term term;
        if (nb::parseTerm(text.toStdString(), &term) == nb::TermResult::Ok)
            key_ = q(nb::keyOfSearchRequest(term));
        problem = session_.nicks().search(text, bot);
    }
    if (!problem.isEmpty()) {
        key_.clear();
        status_->setText(problem);
        return;
    }
    status_->setText(tr("Asked %1. The answer takes a few minutes.").arg(bot_->currentText()));
}

void FindPeopleDialog::showRow(const QString &nick, const QString &address, int botIndex) {
    auto item = new QTreeWidgetItem(results_);
    item->setText(0, nick);
    if (botIndex >= 0) {
        item->setForeground(0, nickbotColor(botIndex));
        item->setIcon(0, dot(nickbotColor(botIndex)));
        item->setToolTip(0, bot_->itemText(botIndex));
    }
    item->setText(1, address);
    item->setFont(1, addressFont());
    item->setText(2, session_.isContact(address) ? tr("yes") : QString());
    item->setData(0, Qt::UserRole, address);
    item->setData(0, Qt::UserRole + 1, nick);
}

void FindPeopleDialog::answered(const QString &kind, const QString &key, const QString &bot,
                                const QVariantMap &result) {
    if (key_.isEmpty() || key != key_ || kind != kind_)
        return;
    const int botIndex = session_.nicks().botIndex(bot);
    key_.clear();
    results_->clear();
    if (result.contains("error")) {
        status_->setText(NickClient::errorText(result["error"].toString()));
        return;
    }
    if (kind == "address") {
        if (result["found"].toBool())
            showRow(result["nick"].toString(), result["address"].toString(), botIndex);
    } else if (kind == "nickname") {
        for (const auto &entry : result["results"].toList())
            for (const auto &nick : entry.toMap()["nicks"].toStringList())
                showRow(nick, entry.toMap()["address"].toString(), botIndex);
    } else {
        for (const auto &hit : result["hits"].toList())
            showRow(hit.toMap()["nick"].toString(), hit.toMap()["address"].toString(), botIndex);
    }
    const int n = results_->topLevelItemCount();
    status_->setText(n == 0 ? tr("%1 knows no one by that.").arg(bot_->currentText())
                     : result["more"].toBool()
                         ? tr("Found: %1. There are more: search for more letters.").arg(n)
                         : tr("Found: %1.").arg(n));
    if (n == 1)
        results_->setCurrentItem(results_->topLevelItem(0));
    updateButtons();
}

void FindPeopleDialog::addSelected() {
    int added = 0;
    for (auto item : results_->selectedItems()) {
        const auto address = item->data(0, Qt::UserRole).toString();
        const auto nick = item->data(0, Qt::UserRole + 1).toString();
        if (!session_.isContact(address) && !session_.addContact(address, nick))
            continue;
        session_.nicks().adopt(address, nick, bot_->currentData().toString());
        item->setText(2, tr("yes"));
        ++added;
    }
    status_->setText(added ? tr("Added to your contacts: %1.").arg(added) : session_.error());
}
} // namespace bm
