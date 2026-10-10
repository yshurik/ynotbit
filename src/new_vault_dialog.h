#pragma once
// A new vault and its first identity, in one dialog: the address is made, and
// shown, before the vault is.
#include "key_search.h"
#include <QDialog>
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTimer;
class QToolButton;
namespace bm {
class Session;
class NewVaultDialog : public QDialog {
    Q_OBJECT
  public:
    // folder: where the vault file goes unless the user picks another place.
    NewVaultDialog(Session &session, const QString &folder, bool dark, QWidget *parent = nullptr);
    ~NewVaultDialog() override;
    // An address's nickname: its name, each word capitalised but "of", "from"
    // and "the" after the first.
    static QString nicknameFor(const QString &address);
    // A file name from a nickname: lower case, letters and digits, one "-" for
    // every other run; "vault" if nothing is left.
    static QString slugFor(const QString &nickname);

  public slots:
    void done(int result) override;

  private:
    Session &session_;
    QString folder_;
    Identity identity_; // the keys shown; their address empty while there are none
    KeySearch search_;
    bool nicknameEdited_ = false, fileEdited_ = false;
    qint64 firstPair_ = -1; // the phrase's own address: its key pair goes unnoted
    QLineEdit *nickname_, *phrase_, *file_, *password_, *repeat_;
    QLabel *identicon_, *address_, *searchStatus_, *rebuild_, *fileProblem_, *passwordProblem_,
        *error_;
    QRadioButton *random_, *fromPhrase_;
    QWidget *phraseRow_;
    QPushButton *shorter_, *create_;
    QToolButton *copy_, *another_;
    QTimer *phraseTimer_;
    void setIdentity(Identity identity, qint64 pair);
    void newRandomKeys();
    void phraseChanged();
    void toggleSearch();
    void stopSearch(bool keepFind = true); // keepFind: show a find not yet taken
    void updateFile();
    void validate();
    void create();
};
} // namespace bm
