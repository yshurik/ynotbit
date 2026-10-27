#include "find_people_dialog.h"
#include "address_delegate.h"
#include "appearance.h"
#include "letter_document.h"
#include "letter_render.h"
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
// The combo's text is the nickbot's name and address; the name alone is what the dialog says when
// it speaks of the nickbot, and is kept beside it.
constexpr int kNameRole = Qt::UserRole + 1;
} // namespace

FindPeopleDialog::FindPeopleDialog(Session &session, QWidget *parent)
    : QDialog(parent), session_(session) {
    setObjectName("findPeopleDialog");
    setWindowTitle(tr("Find people"));
    resize(720, 440);
    auto column = new QVBoxLayout(this);
    auto intro =
        new QLabel(tr("Ask a nickbot. Type a nickname (@ben), a name to search for (at least 4 "
                      "letters) or an address (BM-…). The answer takes a few minutes: you can "
                      "close this window and come back."));
    intro->setWordWrap(true);
    intro->setStyleSheet("color:palette(mid);");
    column->addWidget(intro);
    auto form = new QFormLayout;
    // The fields reach the right edge, also where forms keep them at their size hint (macOS).
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    bot_ = new QComboBox;
    bot_->setObjectName("findPeopleBot");
    const auto bots = session_.nicks().bots();
    for (int i = 0; i < bots.size(); ++i) {
        const auto name = bots[i].label.isEmpty() ? bots[i].address : bots[i].label;
        bot_->addItem(dot(nickbotColor(i)),
                      bots[i].label.isEmpty() ? name : name + " — " + bots[i].address,
                      bots[i].address);
        bot_->setItemData(i, name, kNameRole);
    }
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
    // As the address book's table is: its columns, an identicon for each address, the address in
    // the address font at the size of the text, and (the window's style sheet) its rows.
    results_->setHeaderLabels({tr("Address"), tr("Nickname"), tr("In contacts")});
    results_->setItemDelegate(new AddressDelegate(results_));
    results_->setIconSize(QSize(24, 24));
    results_->setTextElideMode(Qt::ElideMiddle);
    results_->setRootIsDecorated(false);
    results_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    results_->setUniformRowHeights(true);
    results_->setAllColumnsShowFocus(true);
    results_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    results_->header()->resizeSection(1, 150);
    results_->header()->setStretchLastSection(true);
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

// What the address is to the user when it is theirs: their own identity or one of their chans.
QString FindPeopleDialog::ownKind(const QString &address) const {
    for (const auto &v : session_.identities()) {
        const auto identity = v.toMap();
        if (identity["address"].toString() == address)
            return identity["chan"].toBool() ? tr("Your chan") : tr("Your identity");
    }
    return {};
}

void FindPeopleDialog::updateButtons() {
    search_->setEnabled(bot_->count() > 0 && !field_->text().trimmed().isEmpty());
    // Something can be added when a row that is not the user's own is chosen.
    bool addable = false;
    for (auto item : results_->selectedItems())
        addable = addable || ownKind(item->data(0, Qt::UserRole).toString()).isEmpty();
    add_->setEnabled(addable);
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
        problem = session_.nicks().lookupNicknames({text}, bot);
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
    status_->setText(tr("Asked %1. The answer takes a few minutes.")
                         .arg(bot_->currentData(kNameRole).toString()));
}

void FindPeopleDialog::showRow(const QString &nick, const QString &address, int botIndex) {
    auto item = new QTreeWidgetItem(results_);
    const auto ratio = results_->devicePixelRatioF();
    auto identicon = identiconPixmap(address, qRound(results_->iconSize().width() * ratio));
    identicon.setDevicePixelRatio(ratio);
    item->setIcon(0, QIcon(identicon));
    item->setText(0, address);
    item->setText(1, nick);
    // In the colour of the nickbot that vouches for it, which the tip names.
    if (botIndex >= 0) {
        item->setForeground(1, nickbotColor(botIndex));
        item->setToolTip(1, bot_->itemText(botIndex));
    }
    item->setText(2, relation(address));
    item->setData(0, Qt::UserRole, address);
    item->setData(0, Qt::UserRole + 1, nick);
}

// The last column: the user's own identity or chan, a contact already, or nothing.
QString FindPeopleDialog::relation(const QString &address) const {
    if (const auto own = ownKind(address); !own.isEmpty())
        return own;
    return session_.isContact(address) ? tr("yes") : QString();
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
    status_->setText(
        n == 0 ? tr("%1 knows no one by that.").arg(bot_->currentData(kNameRole).toString())
        : result["more"].toBool() ? tr("Found: %1. There are more: search for more letters.").arg(n)
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
        if (!ownKind(address).isEmpty()) // the user's own is no contact
            continue;
        if (!session_.isContact(address) && !session_.addContact(address, nick))
            continue;
        session_.nicks().adopt(address, nick, bot_->currentData().toString());
        item->setText(2, relation(address));
        ++added;
    }
    status_->setText(added ? tr("Added to your contacts: %1.").arg(added) : session_.error());
    updateButtons();
}
} // namespace bm
