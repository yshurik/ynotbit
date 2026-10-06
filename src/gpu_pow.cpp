#include "gpu_pow.h"
#include "gpu_backend.h"
#include <QElapsedTimer>
#include <QtEndian>

namespace bm {
GpuSolver &GpuSolver::instance() {
    // Never destroyed: the driver may already be gone when statics are torn down.
    static auto *solver = new GpuSolver;
    return *solver;
}
GpuSolver::~GpuSolver() = default;

void GpuSolver::open() {
    if (qEnvironmentVariableIsSet("YNOTBIT_NO_GPU")) {
        problem_ = "switched off by YNOTBIT_NO_GPU";
        return;
    }
#ifdef Q_OS_MACOS
    // Metal is Apple's own GPU interface; OpenCL there is deprecated.
    QString metal;
    backend_ = gpu::openMetal(&metal);
    if (!backend_ && !(backend_ = gpu::openOpenCL(&problem_)))
        problem_ = metal + "; " + problem_;
#else
    backend_ = gpu::openOpenCL(&problem_);
#endif
    if (!backend_)
        return;
    device_ = backend_->device();
    ok_ = true;
}
bool GpuSolver::available() {
    std::call_once(opened_, [this] { open(); });
    return ok_;
}
QString GpuSolver::problem() {
    available();
    std::lock_guard lock(use_);
    return problem_;
}
QString GpuSolver::deviceName() {
    available();
    std::lock_guard lock(use_);
    return device_;
}
QString GpuSolver::api() {
    available();
    std::lock_guard lock(use_);
    return backend_ ? backend_->api() : QString();
}
void GpuSolver::disable(const QString &why) {
    std::lock_guard lock(use_);
    ok_ = false;
    problem_ = why;
}
std::optional<quint64> GpuSolver::search(const unsigned char initial[64], quint64 target,
                                         quint64 first, const std::atomic_bool &cancel) {
    if (!available())
        return std::nullopt;
    std::lock_guard lock(use_);
    if (!ok_)
        return std::nullopt;
    quint64 words[8];
    for (int i = 0; i < 8; ++i)
        words[i] = qFromBigEndian<quint64>(initial + 8 * i);
    const auto failed = [&](const QString &error) {
        ok_ = false;
        problem_ = "GPU stopped working (" + error + ")";
        return std::nullopt;
    };
    if (const auto error = backend_->begin(words); !error.isEmpty())
        return failed(error);
    // Short batches keep cancelling quick and stay far below the time after
    // which Windows resets a GPU that does not answer (2 s).
    constexpr qint64 kBatchMs = 30;
    quint64 base = first;
    QElapsedTimer batch;
    while (!cancel) {
        batch.start();
        const auto result = backend_->run(base, perItem_, target);
        if (!result.error.isEmpty())
            return failed(result.error);
        if (result.nonce)
            return result.nonce;
        base += backend_->width() * perItem_;
        const qint64 ms = batch.elapsed();
        if (ms < kBatchMs / 2 && perItem_ < 65536)
            perItem_ *= 2;
        else if (ms > kBatchMs * 2 && perItem_ > 1)
            perItem_ /= 2;
    }
    return std::nullopt;
}
} // namespace bm
