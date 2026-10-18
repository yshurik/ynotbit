#include "key_search.h"
#include "protocol.h"
#include "qt_bridge.h"
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <bmc/phrase.h>
#include <map>
#include <mutex>
#include <openssl/crypto.h>
#include <sodium.h>
#include <thread>
#include <utility>
#include <vector>
namespace bm {
namespace {
// An identity from its two private keys.
Identity fromKeys(const unsigned char sign[32], const unsigned char enc[32]) {
    return bridge::fromCore(bmc::phrase::fromKeys(sign, enc));
}
std::string_view textOf(const QByteArray &phrase) {
    return {phrase.constData(), size_t(phrase.size())};
}
} // namespace
Identity phraseIdentity(const QString &phrase, const QString &label, quint64 pair) {
    bridge::SecretText text(phrase);
    return bridge::fromCore(bmc::phrase::identity(text.value, bridge::toStd(label), pair));
}
std::optional<quint64> firstPair(const QString &phrase, int zeroBytes, quint64 limit) {
    bridge::SecretText text(phrase);
    return bmc::phrase::firstPair(text.value, zeroBytes, limit);
}
int zeroBytes(const QString &address) {
    return bmc::phrase::zeroBytes(bridge::toStd(address));
}
namespace {
constexpr quint64 kChunk = 4096; // pairs a thread takes at a time
} // namespace
struct KeySearch::Run {
    QByteArray phrase; // empty: random keys
    int fromZeroBytes = 1;
    std::atomic<bool> stop{false};
    std::atomic<quint64> nextChunk{0}, pairsDone{0};
    std::mutex lock;
    // A phrase's finished chunks and their hits (pair, zero bytes), until every
    // lower chunk is finished too.
    std::map<quint64, std::vector<std::pair<quint64, int>>> finished;
    quint64 watermark = 0; // every chunk below it is reported
    int reported = 0;      // zero bytes of the best address announced
    KeyFind best;
    std::vector<std::thread> threads;
    ~Run() {
        if (!phrase.isEmpty())
            OPENSSL_cleanse(phrase.data(), size_t(phrase.size()));
    }
};
KeySearch::KeySearch(int threads, QObject *parent)
    : QObject(parent),
      threads_(threads > 0 ? threads : std::max(1, QThread::idealThreadCount() - 1)),
      timer_(new QTimer(this)) {
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (!run_)
            return;
        const auto done = run_->pairsDone.load();
        const auto seconds = std::max<qint64>(1, clock_.restart()) / 1000.0;
        emit progress((done - lastDone_) / seconds);
        lastDone_ = done;
    });
}
KeySearch::~KeySearch() {
    stop();
}
bool KeySearch::running() const {
    return run_ && !run_->threads.empty();
}
void KeySearch::stop() {
    timer_->stop();
    if (!run_)
        return;
    run_->stop = true;
    for (auto &thread : run_->threads)
        thread.join();
    run_->threads.clear();
}
KeyFind KeySearch::take() {
    if (!run_)
        return {};
    std::lock_guard guard(run_->lock);
    return std::exchange(run_->best, KeyFind{});
}
void KeySearch::start(const QString &phrase, int fromZeroBytes) {
    auto run = std::make_unique<Run>();
    run->phrase = phrase.toUtf8();
    run->fromZeroBytes = fromZeroBytes;
    begin(std::move(run));
}
void KeySearch::start(int fromZeroBytes) {
    auto run = std::make_unique<Run>();
    run->fromZeroBytes = fromZeroBytes;
    begin(std::move(run));
}
void KeySearch::begin(std::unique_ptr<Run> next) {
    stop();
    run_ = std::move(next);
    run_->reported = run_->fromZeroBytes - 1;
    auto run = run_.get();
    // found() is emitted on this object's thread, never on a search thread's.
    const auto announce = [this] {
        QMetaObject::invokeMethod(this, [this] { emit found(); }, Qt::QueuedConnection);
    };
    for (int t = 0; t < threads_; ++t)
        run->threads.emplace_back([run, announce] {
            bmc::phrase::Context ctx;
            unsigned char sign[32], enc[32];
            std::vector<std::pair<quint64, int>> hits;
            while (!run->stop) {
                bool news = false;
                if (run->phrase.isEmpty()) {
                    quint64 searched = 0;
                    for (; searched < kChunk && !run->stop; ++searched) {
                        randombytes_buf(sign, 32);
                        randombytes_buf(enc, 32);
                        const int zeros = ctx.pairZeroBytes(sign, enc);
                        if (zeros < run->fromZeroBytes)
                            continue;
                        std::lock_guard guard(run->lock);
                        if (zeros > run->reported) {
                            run->reported = zeros;
                            run->best = {fromKeys(sign, enc), zeros, -1};
                            news = true;
                        }
                    }
                    run->pairsDone += searched;
                } else {
                    const auto chunk = run->nextChunk++;
                    hits.clear();
                    quint64 i = 0;
                    for (; i < kChunk && !run->stop; ++i) {
                        const auto pair = chunk * kChunk + i;
                        bmc::phrase::key(textOf(run->phrase), 2 * pair, sign);
                        bmc::phrase::key(textOf(run->phrase), 2 * pair + 1, enc);
                        if (const int zeros = ctx.pairZeroBytes(sign, enc);
                            zeros >= run->fromZeroBytes)
                            hits.emplace_back(pair, zeros);
                    }
                    if (i < kChunk) // stopped: an unfinished chunk is not reported
                        break;
                    run->pairsDone += kChunk;
                    std::lock_guard guard(run->lock);
                    run->finished[chunk] = hits;
                    // Report in pair order: the first pair at each new level.
                    for (auto it = run->finished.find(run->watermark); it != run->finished.end();
                         it = run->finished.find(run->watermark)) {
                        for (const auto &[pair, zeros] : it->second)
                            if (zeros > run->reported) {
                                run->reported = zeros;
                                bmc::phrase::key(textOf(run->phrase), 2 * pair, sign);
                                bmc::phrase::key(textOf(run->phrase), 2 * pair + 1, enc);
                                run->best = {fromKeys(sign, enc), zeros, qint64(pair)};
                                news = true;
                            }
                        run->finished.erase(it);
                        ++run->watermark;
                    }
                }
                if (news)
                    announce();
            }
            OPENSSL_cleanse(sign, 32);
            OPENSSL_cleanse(enc, 32);
        });
    lastDone_ = 0;
    clock_.start();
    timer_->start();
}
} // namespace bm
