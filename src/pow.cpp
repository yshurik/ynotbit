#include "pow.h"
#include "gpu_pow.h"
#include "qt_bridge.h"
#include <mutex>

namespace bm {
namespace {
// The GPU, when this machine has one, searches beside the CPU workers of the core.
class GpuAdapter : public bmc::PowSolver {
  public:
    std::optional<uint64_t> search(const unsigned char *initial, uint64_t target, uint64_t first,
                                   const std::atomic_bool &cancel) override {
        return GpuSolver::instance().search(initial, target, first, cancel);
    }
    void disable(const std::string &why) override {
        GpuSolver::instance().disable(bridge::fromStd(why));
    }
};
void registerGpu() {
    static std::once_flag once;
    std::call_once(once, [] { bmc::ProofOfWork::setSolver(std::make_shared<GpuAdapter>()); });
}
} // namespace

void ProofOfWork::start(QByteArray object, quint64 trials, quint64 extra) {
    registerGpu();
    core_.start(bridge::toBytes(object), trials, extra);
}
void ProofOfWork::stop() {
    core_.stop();
}
bool ProofOfWork::done() const {
    return core_.done();
}
QByteArray ProofOfWork::take() {
    return bridge::fromBytes(core_.take());
}
unsigned ProofOfWork::workerCount() {
    return bmc::ProofOfWork::workerCount();
}
void ProofOfWork::setGpuEnabled(bool on) {
    bmc::ProofOfWork::setSolverEnabled(on);
}
bool ProofOfWork::gpuEnabled() {
    return bmc::ProofOfWork::solverEnabled();
}
quint64 ProofOfWork::trialValue(quint64 nonce, const unsigned char initial[64]) {
    return bmc::ProofOfWork::trialValue(nonce, initial);
}
bool ProofOfWork::valid(const QByteArray &object, qint64 now, quint64 trials, quint64 extra) {
    return bmc::ProofOfWork::valid(bridge::view(object), now, trials, extra);
}
} // namespace bm
