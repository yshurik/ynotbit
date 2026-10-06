// Measures how smoothly the Subscriptions feed scrolls. Builds a throwaway
// mailbox whose followed sender has posted letters with a photo each (sized
// like real ones: data: URLs of about 64, 88 or 112 KB), opens the feed in the
// real window and scrolls it down and back up a step at a time, timing each
// frame: the scroll, the repaint it causes, the flush.
//
//   feed_scroll_bench [--posts <n>] [--text-only] [--step <px>] [--passes <n>]
//
// --posts      how many posts (20, one page of the feed)
// --text-only  the same posts without their photos (what the cards alone cost)
// Not a test: it prints numbers. QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2
// measures a Retina-sized frame without opening a window; QT_NO_FAST_MOVE=1
// repaints the whole feed at every step, as a Mac window does.
#include "desktop_window.h"
#include "feed_view.h"
#include "letter_document.h"
#include "letter_render.h"
#include "protocol.h"
#include "session.h"
#include <QtWidgets>
#include <algorithm>
#include <iostream>
#include <numeric>

namespace {
// A photo-like picture: smooth light, fine grain, a few shapes -- what JPEG
// sees in a real photo, so the data: URL comes out a realistic size.
QImage photo(int seed) {
    QImage image(1600, 1200, QImage::Format_RGB32);
    QRandomGenerator random(seed);
    for (int y = 0; y < image.height(); ++y) {
        auto line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int grain = int(random.bounded(24)) - 12;
            const int r = 60 + 120 * y / image.height() + 40 * x / image.width() + grain;
            const int g = 90 + 80 * y / image.height() + grain;
            const int b = 170 - 60 * y / image.height() + 30 * x / image.width() + grain;
            line[x] = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        }
    }
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 40; ++i) {
        p.setBrush(QColor::fromHsv(int(random.bounded(360)), 90, 200, 120));
        p.drawEllipse(QPointF(random.bounded(1600), random.bounded(1200)), 20 + random.bounded(160),
                      20 + random.bounded(120));
    }
    return image;
}
struct Stats {
    QList<double> frames;
    QList<int> painted; // rows of the feed repainted per frame
    double at(double q) const {
        auto sorted = frames;
        std::sort(sorted.begin(), sorted.end());
        return sorted.isEmpty() ? 0 : sorted[qMin(int(q * sorted.size()), int(sorted.size()) - 1)];
    }
};
// Counts what a frame repaints: the rows of the feed's cards holder.
class PaintCounter : public QObject {
  public:
    int rows = 0;
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Paint)
            rows += static_cast<QPaintEvent *>(event)->region().boundingRect().height();
        return QObject::eventFilter(watched, event);
    }
};
} // namespace

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitBench");
    app.setApplicationName("FeedScroll");
    const auto args = app.arguments();
    const bool textOnly = args.contains("--text-only");
    const auto value = [&args](const QString &name, int fallback) {
        const int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1].toInt() : fallback;
    };
    const int step = value("--step", 16), passes = qMax(1, value("--passes", 2)),
              posts = value("--posts", bm::FeedView::kPageSize);
    QTemporaryDir temp;

    // A photo per post, each its own.
    QStringList urls;
    if (!textOnly)
        for (int i = 0; i < posts; ++i) {
            const int budget = QList<int>{64, 88, 112}[i % 3];
            urls << bm::imageDataUrl(photo(i), budget * 1024);
        }
    // One decode, on its own: what showing a photo costs the first time.
    for (const auto &url : urls.mid(0, 3)) {
        QElapsedTimer t;
        t.start();
        const auto source = bm::letterImage(QUrl(url));
        std::cout << "photo: " << url.size() / 1024 << " KB data: URL, " << source.width() << "x"
                  << source.height() << " px: decode " << t.nsecsElapsed() / 1e6 << " ms\n";
    }

    const auto news = bm::Protocol::identity("bench news").address;
    bm::Vault vault;
    vault.create(temp.filePath("vault"), "bench password");
    vault.addIdentity("Personal");
    auto key = vault.addMailboxKey();
    bm::Mailbox mailbox;
    mailbox.create(temp.filePath("mailbox"), key, vault.mailboxKey(key));
    mailbox.subscribe(news, "test1");
    for (int i = 0; i < posts; ++i) {
        QString body = "A few lines about the picture below, as a post would have.\n\n"
                       "Taken on Saturday, from the hill above the harbour.";
        if (!textOnly)
            body += "\n\n![photo][img1]\n\nMore next week.\n\n[img1]: " + urls[i];
        mailbox.store("bench-" + QString::number(i), news, news, "Post " + QString::number(i + 1),
                      body, i + 1, "Broadcasts");
    }
    mailbox.close();
    vault.lock();
    QDir().mkpath(temp.filePath("node"));
    {
        QSettings settings(temp.filePath("node/desktop.ini"), QSettings::IniFormat);
        settings.setValue("vault", temp.filePath("vault"));
        settings.setValue("mailbox", temp.filePath("mailbox"));
    }
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, [&] {
        for (auto w : QApplication::topLevelWidgets())
            if (auto d = qobject_cast<QInputDialog *>(w)) {
                d->setTextValue("bench password");
                d->accept();
            }
    });
    responder.start(10);
    bm::Session session(temp.filePath("node"), true);
    session.unlockVault();
    if (!session.mailboxOpen()) {
        std::cerr << "could not open the bench mailbox\n";
        return 1;
    }
    responder.stop();
    bm::DesktopWindow window(session);
    window.resize(1280, 800);
    window.show();
    const auto settle = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    settle(300);
    window.findChild<QListWidget *>("folders")->setCurrentRow(5);
    settle(200);
    // Opening the feed: every card made, its text laid out, its photo shown.
    QElapsedTimer opening;
    opening.start();
    for (auto chip : window.findChildren<QPushButton *>("channelChip"))
        if (chip->text() == "test1")
            chip->click();
    QCoreApplication::sendPostedEvents();
    window.repaint();
    const auto opened = opening.elapsed();
    settle(500);
    auto scroll = window.findChild<QScrollArea *>("feedScroll");
    if (!scroll) {
        std::cerr << "no feed on screen\n";
        return 1;
    }
    PaintCounter counter;
    scroll->widget()->installEventFilter(&counter);
    settle(300);
    auto bar = scroll->verticalScrollBar();
    std::cout << "platform " << QGuiApplication::platformName().toStdString()
              << ", device pixel ratio " << window.devicePixelRatio() << ", feed "
              << scroll->viewport()->width() << "x" << scroll->viewport()->height()
              << ", scroll range " << bar->maximum() << " px, step " << step << " px, " << posts
              << " posts" << (textOnly ? ", text only" : "") << "\nfeed opened in " << opened
              << " ms\n";
    if (bar->maximum() <= 0) {
        std::cerr << "nothing to scroll\n";
        return 1;
    }
    Stats stats;
    const auto frame = [&](int to) {
        counter.rows = 0;
        QElapsedTimer t;
        t.start();
        bar->setValue(to);
        // The repaint the move asked for, as the next frame would run it.
        QCoreApplication::sendPostedEvents();
        stats.frames << t.nsecsElapsed() / 1e6;
        stats.painted << counter.rows;
    };
    const int decodedBefore = bm::pictureDecodes();
    QElapsedTimer total;
    total.start();
    for (int pass = 0; pass < passes; ++pass) {
        for (int v = step; v <= bar->maximum(); v += step)
            frame(v);
        for (int v = bar->maximum() - step; v >= 0; v -= step)
            frame(v);
    }
    const double sum = std::accumulate(stats.frames.begin(), stats.frames.end(), 0.0);
    auto rows = stats.painted;
    std::sort(rows.begin(), rows.end());
    const int over16 = int(std::count_if(stats.frames.begin(), stats.frames.end(),
                                         [](double ms) { return ms > 16.7; }));
    std::cout << stats.frames.size() << " frames in " << total.elapsed() << " ms: mean "
              << sum / stats.frames.size() << " ms, median " << stats.at(0.5) << " ms, p90 "
              << stats.at(0.9) << " ms, max " << stats.at(1.0) << " ms; " << over16
              << " frames over 16.7 ms (60 fps); rows repainted per frame, median "
              << rows[rows.size() / 2]
              << "; pictures decoded while scrolling: " << bm::pictureDecodes() - decodedBefore
              << "\n";
    return 0;
}
