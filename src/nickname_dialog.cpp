#include "nickname_dialog.h"
#include "appearance.h"
#include "letter_render.h"
#include "nick_client.h"
#include "session.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <bmc/nickbot.h>

namespace bm {
namespace {
constexpr auto kProblemStyle = "color:#d93025;";
QLabel *note(const QString &text) {
    auto label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet("color:palette(mid);");
    return label;
}
} // namespace

NicknameDialog::NicknameDialog(Session &session, const QString &address, const QString &label,
                               QWidget *parent)
    : QDialog(parent), session_(session), address_(address) {
    setObjectName("nicknameDialog");
    setWindowTitle(tr("Nickname"));
    setMinimumWidth(460);
    auto &nicks = session.nicks();
    const auto registration = nicks.registration(address);
    const auto state = registration["state"].toString();
    standing_ = nicks.nicknameOf(address).text;
    // The last try, while it is not settled; else the nickname that stands; else one made from the
    // identity's name.
    const bool unsettled = state == "waiting" || state == "refused" || state == "noanswer";
    auto nick = unsettled ? registration["nick"].toString() : standing_;
    if (nick.isEmpty())
        nick = NickClient::suggestNick(label);

    auto column = new QVBoxLayout(this);
    auto who = new QHBoxLayout;
    auto identicon = new QLabel;
    identicon->setFixedSize(40, 40);
    identicon->setPixmap(identiconPixmap(address, 40));
    who->addWidget(identicon);
    auto names = new QVBoxLayout;
    auto name = new QLabel(label);
    name->setObjectName("nicknameIdentityName");
    name->setTextFormat(Qt::PlainText);
    name->setStyleSheet("font-weight:700;");
    names->addWidget(name);
    auto where = new QLabel(address);
    where->setObjectName("nicknameIdentityAddress");
    where->setFont(addressFont());
    where->setTextFormat(Qt::PlainText);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where->setStyleSheet("color:palette(mid);");
    names->addWidget(where);
    who->addLayout(names, 1);
    column->addLayout(who);

    state_ = new QLabel;
    state_->setObjectName("nicknameState");
    state_->setWordWrap(true);
    state_->setTextFormat(Qt::PlainText);
    if (state == "registered") {
        state_->setText(tr("Registered: %1").arg(registration["nick"].toString()));
    } else if (unsettled) {
        state_->setText(NickClient::describe(registration));
        state_->setStyleSheet(state == "waiting" ? "color:palette(mid);" : kProblemStyle);
    }
    state_->setVisible(!state_->text().isEmpty());
    column->addWidget(state_);

    auto form = new QFormLayout;
    field_ = new QLineEdit(nick);
    field_->setObjectName("nicknameField");
    field_->setPlaceholderText("@name");
    form->addRow(tr("Nickname"), field_);
    const auto bots = nicks.bots();
    if (bots.size() > 1) {
        which_ = new QComboBox;
        which_->setObjectName("nicknameBot");
        for (int i = 0; i < bots.size(); ++i) {
            which_->addItem(dotIcon(nickbotColor(i)),
                            bots[i].label.isEmpty() ? bots[i].address : bots[i].label,
                            bots[i].address);
            // The nickbot the last try went to.
            if (state != "none" && bots[i].address == registration["bot"].toString())
                which_->setCurrentIndex(i);
        }
        form->addRow(tr("Nickbot"), which_);
    }
    column->addLayout(form);
    problem_ = new QLabel;
    problem_->setObjectName("nicknameProblem");
    problem_->setWordWrap(true);
    problem_->setStyleSheet(kProblemStyle);
    problem_->hide();
    column->addWidget(problem_);
    column->addWidget(
        note(tr("A nickname is @, / or # followed by a name of 3 to 32 characters: a letter, "
                "then letters, digits, _ or -. # is for chans only.")));
    column->addWidget(
        note(tr("Registering sends one message from the identity to the nickbot, which "
                "then knows the identity's address; the nickname and the address are "
                "public.")));

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    column->addWidget(buttons);
    register_ = buttons->button(QDialogButtonBox::Ok);
    register_->setObjectName("nicknameRegisterButton");
    register_->setText(tr("Register"));
    connect(buttons, &QDialogButtonBox::accepted, this, &NicknameDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(field_, &QLineEdit::textChanged, this, &NicknameDialog::check);
    check();
    field_->selectAll();
    field_->setFocus();
}

// Register is on for a nickname that is good, is not the one that stands, and can be asked for at
// all; what is in the way is said.
void NicknameDialog::check() {
    const auto text = field_->text().trimmed();
    auto problem = session_.nicks().registrationProblem();
    bool good = false;
    if (problem.isEmpty()) {
        const auto nick = bmc::nickbot::parseNick(text.toStdString());
        if (nick && nick->sigil == bmc::nickbot::Sigil::Chan)
            problem = tr("# is for chans. Start with @ for a person or / for a bot.");
        else
            good = nick.has_value() && text != standing_;
    }
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    register_->setEnabled(good);
}

void NicknameDialog::accept() {
    const auto problem = session_.nicks().registerNickname(
        address_, field_->text().trimmed(), which_ ? which_->currentData().toString() : QString());
    if (!problem.isEmpty()) {
        problem_->setText(problem);
        problem_->setVisible(true);
        return;
    }
    QDialog::accept();
}
} // namespace bm
