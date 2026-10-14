#include "join_channel_dialog.h"
#include "appearance.h"
#include "desktop_window.h"
#include <QAction>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
namespace bm {
namespace {
QLabel *note(const QString &text, const QString &name) {
    auto label = new QLabel(text);
    label->setObjectName(name);
    label->setWordWrap(true);
    // Wrapped to the width the fields make, never widening the dialog.
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(policy);
    label->setStyleSheet("color:palette(mid);font-size:12px;");
    return label;
}
// A row under its radio button, indented to the button's text.
QWidget *indented(QVBoxLayout *&column) {
    auto row = new QWidget;
    column = new QVBoxLayout(row);
    column->setContentsMargins(24, 0, 0, 0);
    return row;
}
// "#name" from what is typed, with or without the '#'.
QString hashName(const QString &text) {
    auto name = text.trimmed();
    while (name.startsWith('#'))
        name = name.mid(1).trimmed();
    return name.isEmpty() ? QString() : '#' + name;
}
} // namespace
JoinChannelDialog::JoinChannelDialog(bool dark, QWidget *parent) : QDialog(parent) {
    setObjectName("joinChannelDialog");
    setWindowTitle(tr("Join or create a channel"));
    auto form = new QFormLayout(this);
    form->setSizeConstraint(QLayout::SetFixedSize);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    public_ = new QRadioButton(tr("Public"));
    public_->setObjectName("publicChannelButton");
    public_->setChecked(true);
    QVBoxLayout *publicColumn;
    publicRow_ = indented(publicColumn);
    publicName_ = new QLineEdit;
    publicName_->setObjectName("publicChannelName");
    publicName_->setPlaceholderText(tr("#general"));
    publicColumn->addWidget(publicName_);
    publicColumn->addWidget(note(tr("The name is the shared phrase: anyone who knows the name "
                                    "can read and post."),
                                 "publicChannelNote"));

    secret_ = new QRadioButton(tr("Secret"));
    secret_->setObjectName("secretChannelButton");
    QVBoxLayout *secretColumn;
    secretRow_ = indented(secretColumn);
    secretName_ = new QLineEdit;
    secretName_->setObjectName("secretChannelName");
    secretName_->setPlaceholderText(tr("#name"));
    phrase_ = new QLineEdit;
    phrase_->setObjectName("secretChannelPhrase");
    phrase_->setEchoMode(QLineEdit::Password);
    phrase_->setPlaceholderText(tr("Shared secret phrase"));
    auto showPhrase =
        phrase_->addAction(materialIcon("eye", iconColor(dark)), QLineEdit::TrailingPosition);
    showPhrase->setObjectName("showChannelPhraseAction");
    showPhrase->setCheckable(true);
    showPhrase->setToolTip(tr("Show phrase"));
    connect(showPhrase, &QAction::toggled, this, [this, showPhrase, dark](bool on) {
        phrase_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        showPhrase->setIcon(materialIcon(on ? "eyeOff" : "eye", iconColor(dark)));
        showPhrase->setToolTip(on ? tr("Hide phrase") : tr("Show phrase"));
    });
    secretColumn->addWidget(secretName_);
    secretColumn->addWidget(phrase_);
    secretColumn->addWidget(note(tr("Only people with the exact phrase can read and post. The "
                                    "name is yours alone and never sent."),
                                 "secretChannelNote"));
    // The dialog is as wide as the fields showing, so each asks for the same: an address with
    // room to spare, and the style sheet's 24 px. Public or secret, the dialog keeps its width.
    const int fieldWidth = QFontMetrics(addressFont()).horizontalAdvance(QString(46, '0')) + 24;
    for (auto field : {publicName_, secretName_, phrase_})
        field->setMinimumWidth(fieldWidth);

    auto kind = new QVBoxLayout;
    kind->addWidget(public_);
    kind->addWidget(publicRow_);
    kind->addWidget(secret_);
    kind->addWidget(secretRow_);
    form->addRow(tr("Channel"), kind);

    address_ = new QLineEdit;
    address_->setObjectName("channelAddressField");
    address_->setFont(addressFont());
    address_->setPlaceholderText(tr("BM-address, optional"));
    auto addressColumn = new QVBoxLayout;
    addressColumn->addWidget(address_);
    addressColumn->addWidget(note(tr("To join an existing chan, its address checks the phrase. "
                                     "Left empty, the phrase makes a version 4 chan."),
                                  "channelAddressNote"));
    form->addRow(tr("Address"), addressColumn);

    preview_ = note({}, "channelPreview");
    form->addRow(QString(), preview_);

    auto cancel = new QPushButton(DesktopWindow::tr("Cancel"));
    cancel->setObjectName("joinChannelCancelButton");
    join_ = new QPushButton(tr("Join"));
    join_->setObjectName("joinChannelAcceptButton");
    makeDefault(join_);
    auto buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(join_);
    form->addRow(buttons);

    for (auto field : {publicName_, secretName_, phrase_, address_})
        connect(field, &QLineEdit::textChanged, this, &JoinChannelDialog::update);
    connect(public_, &QRadioButton::toggled, this, [this](bool on) {
        update();
        (on ? publicName_ : secretName_)->setFocus();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(join_, &QPushButton::clicked, this, &QDialog::accept);
    publicName_->setFocus();
    update();
}
JoinChannelDialog::~JoinChannelDialog() {
    phrase_->clear();
}
QString JoinChannelDialog::phrase() const {
    // A public chan's phrase is its name without the '#'.
    return public_->isChecked() ? hashName(publicName_->text()).mid(1) : phrase_->text();
}
QString JoinChannelDialog::name() const {
    return hashName((public_->isChecked() ? publicName_ : secretName_)->text());
}
QString JoinChannelDialog::expectedAddress() const {
    return address_->text().trimmed();
}
void JoinChannelDialog::update() {
    const bool open = public_->isChecked();
    publicRow_->setVisible(open);
    secretRow_->setVisible(!open);
    const auto shown = name();
    preview_->setText(shown.isEmpty() ? QString() : tr("Listed as %1").arg(shown));
    preview_->setVisible(!shown.isEmpty());
    join_->setEnabled(!shown.isEmpty() && (open || !phrase_->text().isEmpty()));
}
} // namespace bm
