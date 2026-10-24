#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include <QStringList>
#include <bmc/pybm_import.h>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
namespace bm {
class Session;

// Imports the identities, chans, address book, subscriptions and letters of a PyBitmessage data
// folder into the open vault and mailbox: choose what to bring (with the counts), watch it go, read
// what became of everything. The work itself is bmc::pybm::Importer's, a turn of the event loop at
// a time, so the window stays alive and can stop it.
class PyBitmessageImportDialog : public QDialog {
    Q_OBJECT
  public:
    // `folders` are the places PyBitmessage's data may be; the first is shown.
    PyBitmessageImportDialog(Session &session, const QStringList &folders,
                             QWidget *parent = nullptr);
    ~PyBitmessageImportDialog() override;
    // Where PyBitmessage keeps its data on this system, those folders that hold some.
    static QStringList defaultFolders();
    // "about 3 min left"; while the answer is not known (seconds < 0) it says so.
    static QString timeLeftText(double seconds);
    // Rows per chunk, and a pause between two turns of the event loop (a test makes it slow enough
    // to be watched); each turn then does one chunk.
    void setPaceForTesting(int chunk, int pauseMs);
    bool running() const {
        return importer_ != nullptr && !finished_;
    }

  public slots:
    void reject() override; // while an import goes, stops it instead of closing

  private:
    enum Page { Choose, Progress, Result };
    void showPage(Page page);
    void updateTrashHint();
    void chooseFolder(int index);
    void showFolder(const QString &folder);
    void start();
    void pump();
    void finish();
    void stop();
    QString details() const;
    Session &session_;
    QStackedWidget *pages_;
    QComboBox *folder_;
    QCheckBox *identities_, *contacts_, *subscriptions_, *inbox_, *sent_, *trash_;
    QLabel *identitiesCount_, *contactsCount_, *subscriptionsCount_, *inboxCount_, *sentCount_,
        *trashCount_, *trashHint_, *notes_, *trouble_, *phase_, *detail_, *resultTitle_,
        *resultText_;
    QPushButton *import_, *checkAgain_, *stop_, *copy_, *close_;
    QProgressBar *bar_;
    QTimer *timer_;
    bmc::pybm::Preview preview_;
    std::unique_ptr<bmc::pybm::Importer> importer_;
    bmc::pybm::Estimator estimator_;
    QElapsedTimer clock_;
    int chunk_ = 0, budgetMs_ = 60, pauseMs_ = 0;
    bool finished_ = false, stopping_ = false;
    QString shownFolder_;
};
} // namespace bm
