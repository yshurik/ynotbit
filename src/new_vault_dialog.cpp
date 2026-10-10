#include "new_vault_dialog.h"
#include "appearance.h"
#include "coolname.h"
#include "letter_render.h"
#include "protocol.h"
#include "session.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QStyleOptionButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace bm {
namespace {
QLabel *note(const QString &text, const QString &name, bool problem = false) {
    auto label = new QLabel(text);
    label->setObjectName(name);
    label->setWordWrap(true);
    // Wrapped to the width the fields make, never widening the dialog.
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(policy);
    label->setStyleSheet(problem ? "color:#d93025;font-size:12px;"
                                 : "color:palette(mid);font-size:12px;");
    return label;
}
// A section's heading, bold as on the Settings page; room above it when it
// follows another section.
QLabel *section(const QString &text, int above) {
    auto label = new QLabel(text);
    auto font = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setContentsMargins(0, above, 0, 0);
    return label;
}
// A button as wide as the widest of the texts it shows in turn, so its row
// stays put as they change.
class SteadyButton : public QPushButton {
  public:
    explicit SteadyButton(QStringList texts) : texts_(std::move(texts)) {
        setText(texts_.value(0));
    }
    QSize sizeHint() const override {
        auto hint = QPushButton::sizeHint();
        QStyleOptionButton option;
        initStyleOption(&option);
        for (const auto &text : texts_) {
            option.text = text;
            hint = hint.expandedTo(
                style()->sizeFromContents(QStyle::CT_PushButton, &option,
                                          fontMetrics().size(Qt::TextShowMnemonic, text), this));
        }
        return hint;
    }

  private:
    QStringList texts_;
};
// About how long, said briefly.
QString duration(double seconds) {
    if (seconds < 60)
        return NewVaultDialog::tr("%1 s").arg(std::max(1, int(seconds)));
    if (seconds < 3600)
        return NewVaultDialog::tr("%1 min").arg(int(seconds / 60));
    if (seconds < 86400)
        return NewVaultDialog::tr("%1 h").arg(int(seconds / 3600));
    return NewVaultDialog::tr("%1 days").arg(int(seconds / 86400));
}
} // namespace
QString NewVaultDialog::nicknameFor(const QString &address) {
    auto words = addressName(address);
    for (qsizetype i = 0; i < words.size(); ++i)
        if (i == 0 || (words[i] != "of" && words[i] != "from" && words[i] != "the"))
            words[i][0] = words[i][0].toUpper();
    return words.join(' ');
}
QString NewVaultDialog::slugFor(const QString &nickname) {
    QString slug;
    for (const auto c : nickname.toLower()) {
        if (c.isLetterOrNumber())
            slug += c;
        else if (!slug.isEmpty() && !slug.endsWith('-'))
            slug += '-';
    }
    while (slug.endsWith('-'))
        slug.chop(1);
    return slug.isEmpty() ? QString("vault") : slug;
}
NewVaultDialog::NewVaultDialog(Session &session, const QString &folder, bool dark, QWidget *parent)
    : QDialog(parent), session_(session), folder_(folder), phraseTimer_(new QTimer(this)) {
    setObjectName("newVaultDialog");
    setWindowTitle(tr("New vault"));
    auto form = new QFormLayout(this);
    // As big as its rows, so none is squeezed as notes come and go; the fields
    // reach the right edge, also where forms keep them at their size hint.
    form->setSizeConstraint(QLayout::SetFixedSize);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(section(tr("Identity"), 0));
    nickname_ = new QLineEdit;
    nickname_->setObjectName("nicknameField");
    form->addRow(tr("Nickname"), nickname_);
    // The address, with its identicon and buttons, and the search for a
    // shorter one.
    identicon_ = new QLabel;
    identicon_->setFixedSize(32, 32);
    address_ = new QLabel;
    address_->setObjectName("addressLabel");
    address_->setFont(addressFont());
    address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // As wide as the longest address: the dialog keeps its width while one is made.
    address_->setMinimumWidth(QFontMetrics(addressFont()).horizontalAdvance(QString(38, '0')));
    const auto addressButton = [dark](const QString &name, const QString &icon,
                                      const QString &tip) {
        auto button = new QToolButton;
        button->setObjectName(name);
        button->setProperty("addressAction", true);
        button->setFixedSize(22, 22);
        button->setIconSize(QSize(14, 14));
        button->setIcon(materialIcon(icon, iconColor(dark)));
        button->setToolTip(tip);
        return button;
    };
    copy_ = addressButton("copyNewAddressButton", "copy", tr("Copy address"));
    connect(copy_, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(identity_.address); });
    another_ = addressButton("anotherKeysButton", "retry", tr("Another random address"));
    auto addressRow = new QHBoxLayout;
    addressRow->addWidget(identicon_);
    addressRow->addWidget(address_);
    addressRow->addWidget(copy_);
    addressRow->addWidget(another_);
    addressRow->addStretch(1);
    shorter_ = new SteadyButton({tr("Find a shorter address"), tr("Stop search")});
    shorter_->setObjectName("shorterButton");
    searchStatus_ =
        note(tr("Optional. Shorter addresses are easier to read and type."), "searchStatus");
    auto shorterRow = new QHBoxLayout;
    shorterRow->addWidget(shorter_);
    shorterRow->addWidget(searchStatus_, 1);
    auto addressColumn = new QVBoxLayout;
    addressColumn->addLayout(addressRow);
    addressColumn->addLayout(shorterRow);
    form->addRow(tr("Address"), addressColumn);
    // Where the keys come from.
    random_ = new QRadioButton(tr("Random, from the system's entropy"));
    random_->setObjectName("randomKeysButton");
    random_->setChecked(true);
    fromPhrase_ = new QRadioButton(tr("From a secret phrase"));
    fromPhrase_->setObjectName("phraseKeysButton");
    phrase_ = new QLineEdit;
    phrase_->setObjectName("phraseField");
    phrase_->setEchoMode(QLineEdit::Password);
    phrase_->setPlaceholderText(tr("Secret phrase"));
    auto showPhrase =
        phrase_->addAction(materialIcon("eye", iconColor(dark)), QLineEdit::TrailingPosition);
    showPhrase->setObjectName("showPhraseAction");
    showPhrase->setCheckable(true);
    showPhrase->setToolTip(tr("Show phrase"));
    connect(showPhrase, &QAction::toggled, this, [this, showPhrase, dark](bool on) {
        phrase_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        showPhrase->setIcon(materialIcon(on ? "eyeOff" : "eye", iconColor(dark)));
        showPhrase->setToolTip(on ? tr("Hide phrase") : tr("Show phrase"));
    });
    phraseRow_ = new QWidget;
    auto phraseColumn = new QVBoxLayout(phraseRow_);
    phraseColumn->setContentsMargins(24, 0, 0, 0);
    phraseColumn->addWidget(phrase_);
    phraseColumn->addWidget(note(tr("The same phrase always gives the same address. Anyone who "
                                    "knows or guesses it has your keys: use a long random phrase."),
                                 "phraseWarning"));
    rebuild_ = note({}, "rebuildNote");
    rebuild_->hide();
    phraseColumn->addWidget(rebuild_);
    phraseRow_->hide();
    auto keys = new QVBoxLayout;
    keys->addWidget(random_);
    keys->addWidget(fromPhrase_);
    keys->addWidget(phraseRow_);
    form->addRow(tr("Keys"), keys);
    // The vault file.
    form->addRow(section(tr("Vault"), 10));
    file_ = new QLineEdit;
    file_->setObjectName("vaultFileField");
    auto choose = new QPushButton("…");
    choose->setObjectName("vaultFileButton");
    choose->setToolTip(tr("Choose where to save the vault"));
    // As tall as the field beside it.
    choose->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto fileRow = new QHBoxLayout;
    fileRow->addWidget(file_, 1);
    fileRow->addWidget(choose);
    fileProblem_ = note(tr("This file exists; existing vaults are never overwritten."),
                        "vaultFileProblem", true);
    fileProblem_->hide();
    auto fileColumn = new QVBoxLayout;
    fileColumn->addLayout(fileRow);
    fileColumn->addWidget(fileProblem_);
    form->addRow(tr("Vault file"), fileColumn);
    // Its password.
    password_ = new QLineEdit;
    password_->setObjectName("newVaultPasswordField");
    password_->setEchoMode(QLineEdit::Password);
    repeat_ = new QLineEdit;
    repeat_->setObjectName("newVaultRepeatField");
    repeat_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Vault password"), password_);
    passwordProblem_ = note(tr("The passwords do not match."), "passwordProblem", true);
    // Its room is kept, so the buttons stay put as it comes and goes.
    auto kept = passwordProblem_->sizePolicy();
    kept.setRetainSizeWhenHidden(true);
    passwordProblem_->setSizePolicy(kept);
    passwordProblem_->hide();
    auto repeatColumn = new QVBoxLayout;
    repeatColumn->addWidget(repeat_);
    repeatColumn->addWidget(
        note(tr("You need it to open the vault. A forgotten password cannot be recovered."),
             "passwordNote"));
    repeatColumn->addWidget(passwordProblem_);
    form->addRow(tr("Repeat password"), repeatColumn);
    error_ = note({}, "newVaultError", true);
    error_->hide();
    form->addRow(error_);
    auto cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName("newVaultCancelButton");
    create_ = new QPushButton(tr("Create vault"));
    create_->setObjectName("newVaultCreateButton");
    create_->setDefault(true);
    // Measured in the bold the window's style sheet draws a default button in.
    auto bold = create_->font();
    bold.setWeight(QFont::DemiBold);
    create_->setFont(bold);
    auto buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(create_);
    form->addRow(buttons);

    connect(nickname_, &QLineEdit::textEdited, this, [this] {
        nicknameEdited_ = true;
        updateFile();
    });
    connect(random_, &QRadioButton::toggled, this, [this](bool on) {
        phraseRow_->setVisible(!on);
        another_->setVisible(on);
        if (on)
            newRandomKeys();
        else
            phraseChanged();
    });
    connect(another_, &QToolButton::clicked, this, &NewVaultDialog::newRandomKeys);
    phraseTimer_->setSingleShot(true);
    phraseTimer_->setInterval(300);
    // While the phrase changes there is no address: never the last phrase's keys,
    // nor a late find of its search.
    connect(phrase_, &QLineEdit::textChanged, this, [this] {
        stopSearch(false);
        setIdentity(Identity(), -1);
        phraseTimer_->start();
    });
    connect(phraseTimer_, &QTimer::timeout, this, &NewVaultDialog::phraseChanged);
    connect(shorter_, &QPushButton::clicked, this, &NewVaultDialog::toggleSearch);
    connect(&search_, &KeySearch::found, this, [this] {
        if (auto find = search_.take(); !find.identity.address.isEmpty())
            setIdentity(std::move(find.identity), find.pair);
    });
    connect(&search_, &KeySearch::progress, this, [this](double rate) {
        const double pairs = std::pow(256.0, zeroBytes(identity_.address) + 1);
        searchStatus_->setText(
            tr("Trying %1 key pairs a second; next shorter address in about %2.")
                .arg(QLocale().toString(qRound64(rate)), duration(pairs / std::max(rate, 1.0))));
    });
    connect(file_, &QLineEdit::textEdited, this, [this] {
        fileEdited_ = true;
        validate();
    });
    connect(choose, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, tr("Save the vault as"), file_->text(),
                                                 tr("Bitmessage vault (*.bmvault)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(".bmvault"))
            path += ".bmvault";
        file_->setText(path);
        fileEdited_ = true;
        validate();
    });
    for (auto field : {password_, repeat_})
        connect(field, &QLineEdit::textChanged, this, &NewVaultDialog::validate);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(create_, &QPushButton::clicked, this, &NewVaultDialog::create);
    newRandomKeys();
    // Wide enough for a whole default path: the folder, and a long nickname;
    // measured in the style sheet's font.
    file_->ensurePolished();
    const auto longest = QDir(folder_).filePath(QString(30, 'n') + ".bmvault");
    file_->setMinimumWidth(
        std::clamp(file_->fontMetrics().horizontalAdvance(longest) + 24, 360, 600));
}
NewVaultDialog::~NewVaultDialog() = default;
void NewVaultDialog::setIdentity(Identity identity, qint64 pair) {
    identity_ = std::move(identity);
    const auto &address = identity_.address;
    address_->setText(address.isEmpty() ? QString("—") : address);
    identicon_->setPixmap(address.isEmpty() ? QPixmap() : identiconPixmap(address, 32));
    copy_->setEnabled(!address.isEmpty());
    shorter_->setEnabled(!address.isEmpty());
    // The phrase alone gives its first address; any other needs its key pair.
    rebuild_->setText(
        tr("Key pair %1 of this phrase: note the number with the phrase to rebuild this "
           "address.")
            .arg(pair));
    rebuild_->setVisible(pair >= 0 && pair != firstPair_);
    if (!nicknameEdited_ && !address.isEmpty())
        nickname_->setText(nicknameFor(address));
    updateFile();
}
void NewVaultDialog::newRandomKeys() {
    stopSearch();
    setIdentity(Protocol::identity({}), -1);
}
void NewVaultDialog::phraseChanged() {
    stopSearch();
    const auto phrase = phrase_->text();
    const auto pair = phrase.isEmpty() ? std::nullopt : firstPair(phrase, 1);
    firstPair_ = pair ? qint64(*pair) : -1;
    if (pair)
        setIdentity(phraseIdentity(phrase, {}, *pair), qint64(*pair));
    else
        setIdentity(Identity(), -1);
}
void NewVaultDialog::toggleSearch() {
    if (search_.running()) {
        stopSearch();
        return;
    }
    const int from = zeroBytes(identity_.address) + 1;
    if (fromPhrase_->isChecked())
        search_.start(phrase_->text(), from);
    else
        search_.start(from);
    shorter_->setText(tr("Stop search"));
    searchStatus_->setText(tr("Searching…"));
}
void NewVaultDialog::stopSearch(bool keepFind) {
    if (!search_.running())
        return;
    search_.stop();
    shorter_->setText(tr("Find a shorter address"));
    searchStatus_->setText(tr("Optional. Shorter addresses are easier to read and type."));
    // A find announced but not yet taken.
    if (auto find = search_.take(); keepFind && !find.identity.address.isEmpty())
        setIdentity(std::move(find.identity), find.pair);
}
void NewVaultDialog::updateFile() {
    if (!fileEdited_) {
        const auto slug = slugFor(nickname_->text());
        auto path = QDir(folder_).filePath(slug + ".bmvault");
        for (int n = 2; QFileInfo::exists(path); ++n)
            path = QDir(folder_).filePath(QString("%1-%2.bmvault").arg(slug).arg(n));
        file_->setText(path);
    }
    validate();
}
void NewVaultDialog::validate() {
    const auto path = file_->text().trimmed();
    const bool exists = !path.isEmpty() && QFileInfo::exists(path);
    fileProblem_->setVisible(exists);
    const auto password = password_->text(), repeat = repeat_->text();
    // A mismatch is said once the repeat is typed out, not while it is typed.
    passwordProblem_->setVisible(!repeat.isEmpty() && repeat.size() >= password.size() &&
                                 repeat != password);
    create_->setEnabled(!identity_.address.isEmpty() && !path.isEmpty() && !exists &&
                        !password.isEmpty() && password == repeat);
}
void NewVaultDialog::create() {
    stopSearch();
    const auto nickname = nickname_->text().trimmed();
    identity_.label = nickname.isEmpty() ? identity_.address : nickname;
    if (session_.createVault(file_->text().trimmed(), password_->text(), repeat_->text(),
                             identity_)) {
        accept();
        return;
    }
    error_->setText(session_.error());
    error_->show();
    validate();
}
void NewVaultDialog::done(int result) {
    search_.stop();
    {
        const QSignalBlocker block(phrase_);
        phrase_->clear();
    }
    password_->clear();
    repeat_->clear();
    QDialog::done(result);
}
} // namespace bm
