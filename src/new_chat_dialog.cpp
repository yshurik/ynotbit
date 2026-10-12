#include "new_chat_dialog.h"
#include "appearance.h"
#include "desktop_window.h"
#include "letter_render.h"
#include "session.h"
#include <QComboBox>
#include <QCompleter>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>
namespace bm {
NewChatDialog::NewChatDialog(Session &session, bool dark, QWidget *parent)
    : QDialog(parent), session_(session) {
    setObjectName("newChatDialog");
    setWindowTitle(DesktopWindow::tr("New chat"));
    auto form = new QFormLayout(this);
    // As big as its rows; the fields reach the right edge, also where forms keep them at
    // their size hint.
    form->setSizeConstraint(QLayout::SetFixedSize);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    // To: an address, with the address book at hand as in a letter: completion on name or
    // address, and a button listing every contact.
    peer_ = new QLineEdit;
    peer_->setObjectName("chatPeerField");
    peer_->setFont(addressFont());
    peer_->setPlaceholderText(DesktopWindow::tr("Recipient · BM-address"));
    // As wide as an address.
    peer_->setMinimumWidth(QFontMetrics(addressFont()).horizontalAdvance(QString(38, '0')) + 24);
    const auto contacts = session.contacts();
    auto completions = new QStandardItemModel(this);
    auto menu = new QMenu(this);
    menu->setObjectName("chatContactsMenu");
    for (const auto &value : contacts) {
        const auto contact = value.toMap();
        const auto address = contact["address"].toString(), label = contact["label"].toString();
        auto item = new QStandardItem(label + " — " + address);
        item->setData(address, Qt::UserRole);
        completions->appendRow(item);
        menu->addAction(QIcon(identiconPixmap(address, 18)), label, this,
                        [this, address] { peer_->setText(address); });
    }
    auto completer = new QCompleter(completions, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    peer_->setCompleter(completer);
    // Choosing "Name — BM-…" leaves just the address in the field.
    connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated), this,
            [this](const QModelIndex &index) {
                const auto address = index.data(Qt::UserRole).toString();
                QTimer::singleShot(0, this, [this, address] { peer_->setText(address); });
            });
    auto picker = new QPushButton;
    picker->setObjectName("chatContactsButton");
    picker->setIcon(materialIcon("contacts", iconColor(dark)));
    picker->setIconSize(QSize(18, 18));
    picker->setToolTip(DesktopWindow::tr("Choose from contacts"));
    picker->setStyleSheet("QPushButton::menu-indicator{image:none;width:0;}");
    picker->setMenu(menu);
    picker->setVisible(!contacts.isEmpty());
    auto toRow = new QHBoxLayout;
    toRow->setSpacing(6);
    toRow->addWidget(peer_, 1);
    toRow->addWidget(picker);
    form->addRow(DesktopWindow::tr("To:"), toRow);
    problem_ = new QLabel;
    problem_->setObjectName("chatProblem");
    problem_->setWordWrap(true);
    auto policy = problem_->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored); // wrapped to the fields' width
    problem_->setSizePolicy(policy);
    problem_->setStyleSheet("color:#d93025;font-size:12px;");
    problem_->hide();
    form->addRow(QString(), problem_);
    // From: the identity the chat is under. Asked only when there is a choice.
    identity_ = new QComboBox;
    identity_->setObjectName("chatIdentityBox");
    QString preferred;
    for (const auto &value : session.identities()) {
        const auto identity = value.toMap();
        if (identity["chan"].toBool())
            continue;
        const auto address = identity["address"].toString(), label = identity["label"].toString();
        identity_->addItem(QIcon(identiconPixmap(address, 18)), label.isEmpty() ? address : label,
                           address);
        if (identity["default"].toBool())
            preferred = address;
    }
    if (!preferred.isEmpty())
        identity_->setCurrentIndex(identity_->findData(preferred));
    form->addRow(DesktopWindow::tr("From:"), identity_);
    form->setRowVisible(identity_, identity_->count() > 1);
    auto cancel = new QPushButton(DesktopWindow::tr("Cancel"));
    cancel->setObjectName("newChatCancelButton");
    start_ = new QPushButton(tr("Start chat"));
    start_->setObjectName("startChatButton");
    makeDefault(start_);
    start_->setEnabled(false);
    auto buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(start_);
    form->addRow(buttons);
    connect(peer_, &QLineEdit::textChanged, this, [this] { validate(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(start_, &QPushButton::clicked, this, &QDialog::accept);
    validate();
}
QString NewChatDialog::peer() const {
    return peer_->text().trimmed();
}
QString NewChatDialog::identity() const {
    return identity_->currentData().toString();
}
// Said once something is typed; a chat starts only with an address that is someone else's.
void NewChatDialog::validate() {
    const auto address = peer();
    auto problem = address.isEmpty() ? QString() : session_.contactProblem(address);
    if (problem.isEmpty() && identity().isEmpty())
        problem = tr("Create an identity before starting a chat");
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    start_->setEnabled(!address.isEmpty() && problem.isEmpty());
}
} // namespace bm
