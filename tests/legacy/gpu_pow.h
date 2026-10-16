// A GPU that is never there: the old pow.cpp then searches on the CPU alone, as bmclient does when
// no outside solver is registered.
#pragma once
#include <QString>
#include <atomic>
#include <optional>

namespace legacy {
class GpuSolver {
  public:
    static GpuSolver &instance() {
        static GpuSolver solver;
        return solver;
    }
    std::optional<quint64> search(const unsigned char *, quint64, quint64,
                                  const std::atomic_bool &) {
        return std::nullopt;
    }
    void disable(const QString &) {}
};
} // namespace legacy
