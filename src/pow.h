#pragma once
#include <QByteArray>
#include <bmc/pow.h>

namespace bm {
class ProofOfWork {
    bmc::ProofOfWork core_;

  public:
    // Searches on workerCount() threads; done() turns true when one finds a nonce.
    void start(QByteArray object, quint64 trials = 1000, quint64 extra = 1000);
    void stop();
    bool done() const;
    QByteArray take();
    // Every core but one, so the window stays responsive while a letter is prepared.
    static unsigned workerCount();
    // A GPU (GpuSolver) searches alongside the CPU workers when one is usable.
    static void setGpuEnabled(bool on);
    static bool gpuEnabled();
    // The value the network compares with the target for this nonce.
    static quint64 trialValue(quint64 nonce, const unsigned char initial[64]);
    static bool valid(const QByteArray &, qint64 now, quint64 trials = 1000, quint64 extra = 1000);
};
} // namespace bm
