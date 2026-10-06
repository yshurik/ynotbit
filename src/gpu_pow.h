#pragma once
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
namespace bm {
namespace gpu {
class Backend;
}
// Proof of work on a GPU, when the system has one: through Metal on macOS,
// through OpenCL elsewhere (and on a Mac whose Metal fails). OpenCL is loaded
// at run time from the graphics driver, so nothing is linked or shipped for it,
// and a machine without a GPU simply gets available() == false; the CPU search
// in ProofOfWork then does the work alone. Every nonce found here is checked
// again on the CPU before it is used.
class GpuSolver {
  public:
    static GpuSolver &instance();
    // Opens OpenCL and compiles the kernel on first use (about a second).
    bool available();
    // Why the GPU is not used, for diagnostics; empty when it is.
    QString problem();
    QString deviceName();
    // "Metal" or "OpenCL"; empty without a GPU.
    QString api();
    // Searches nonces from `first` upward for one whose trial value is at most
    // `target`, in batches of about 30 ms, until found or `cancel` is set.
    // One search at a time; a second caller waits for the first.
    std::optional<quint64> search(const unsigned char initial[64], quint64 target, quint64 first,
                                  const std::atomic_bool &cancel);
    // After a wrong answer the GPU is not trusted again in this run.
    void disable(const QString &why);
    ~GpuSolver();

  private:
    GpuSolver() = default;
    void open();
    std::once_flag opened_;
    std::mutex use_;
    std::unique_ptr<gpu::Backend> backend_;
    quint32 perItem_ = 16; // nonces per work item, tuned towards short batches
    QString problem_, device_;
    std::atomic_bool ok_{false};
};
} // namespace bm
