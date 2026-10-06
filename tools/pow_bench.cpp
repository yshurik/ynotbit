// How fast proof of work runs on this machine: nonces per second on the CPU
// workers and on each GPU backend, and what that means for sending a letter.
//   cmake --build build --target pow_bench && build/pow_bench
#include "gpu_backend.h"
#include "pow.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtEndian>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr qint64 kMeasureMs = 2000;
struct Rate {
    std::string name;
    double perSecond;
};
// Nonces a message object needs on average: the network's default difficulty
// (1000 trials per byte, 1000 extra bytes) over a time to live, as pow.cpp.
double expectedNonces(double bytes, double ttlSeconds) {
    const double length = bytes + 1000;
    return 1000 * (length + length * ttlSeconds / 65536);
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    unsigned char initial[64];
    for (int i = 0; i < 64; ++i)
        initial[i] = static_cast<unsigned char>(i * 29 + 3);
    quint64 words[8];
    for (int i = 0; i < 8; ++i)
        words[i] = qFromBigEndian<quint64>(initial + 8 * i);
    std::vector<Rate> rates;

    // The CPU, as ProofOfWork searches: every core but one.
    {
        const unsigned count = bm::ProofOfWork::workerCount();
        std::atomic_bool stop{false};
        std::atomic<quint64> tried{0}, keep{0};
        std::vector<std::thread> workers;
        QElapsedTimer clock;
        clock.start();
        for (unsigned i = 0; i < count; ++i)
            workers.emplace_back([&, i] {
                quint64 n = 0, sink = 0;
                for (quint64 nonce = i; !stop; nonce += count, ++n)
                    sink ^= bm::ProofOfWork::trialValue(nonce, initial);
                tried += n;
                keep ^= sink; // the values are used, so the work stays
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(kMeasureMs));
        stop = true;
        for (auto &worker : workers)
            worker.join();
        rates.push_back(
            {"CPU, " + std::to_string(count) + " threads", tried * 1000.0 / clock.elapsed()});
    }
    // Each GPU backend, in batches of about 30 ms as GpuSolver runs them.
    const auto measure = [&](std::unique_ptr<bm::gpu::Backend> gpu, const QString &problem,
                             const char *api) {
        if (!gpu) {
            std::printf("%s: %s\n", api, problem.toLocal8Bit().constData());
            return;
        }
        gpu->begin(words);
        quint32 perItem = 16;
        quint64 base = 0, tried = 0;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < kMeasureMs) {
            QElapsedTimer batch;
            batch.start();
            const auto result = gpu->run(base, perItem, 0);
            if (!result.error.isEmpty()) {
                std::printf("%s failed: %s\n", api, result.error.toLocal8Bit().constData());
                return;
            }
            base += gpu->width() * perItem;
            tried += gpu->width() * perItem;
            if (batch.elapsed() < 15 && perItem < 65536)
                perItem *= 2;
            else if (batch.elapsed() > 60 && perItem > 1)
                perItem /= 2;
        }
        rates.push_back({gpu->api().toStdString() + " (" + gpu->device().toStdString() + ")",
                         tried * 1000.0 / clock.elapsed()});
    };
    QString problem;
    measure(bm::gpu::openOpenCL(&problem), problem, "OpenCL");
#ifdef Q_OS_MACOS
    measure(bm::gpu::openMetal(&problem), problem, "Metal");
#endif

    for (const auto &rate : rates)
        std::printf("%-28s %8.1f million nonces/s\n", rate.name.c_str(), rate.perSecond / 1e6);
    // ProofOfWork runs the CPU workers and the solver's GPU together.
    const double cpu = rates.front().perSecond,
                 best = rates.size() > 1 ? rates.back().perSecond : 0;
    const double ttl = 4 * 86400;
    for (const auto &[what, bytes] : {std::pair{"Plain letter, 1 KB", 1024.0},
                                      std::pair{"Letter with a 160 KB picture", 170000.0}}) {
        const double nonces = expectedNonces(bytes, ttl);
        std::printf("%s, 4-day TTL: %.3g nonces on average; CPU alone %.1f s, CPU + %s %.1f s\n",
                    what, nonces, nonces / cpu,
                    rates.size() > 1 ? rates.back().name.c_str() : "GPU", nonces / (cpu + best));
    }
    return 0;
}
