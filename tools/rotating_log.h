#pragma once
// A log file for a service that runs for months: it keeps to a size. When the next line would
// pass maxBytes, log becomes log.1, log.1 becomes log.2, and so on, and the oldest of the kept
// ones goes. A log that already exists is gone on with.
#include <cstdint>
#include <cstdio>
#include <string>

namespace tools {
class RotatingLog {
  public:
    explicit RotatingLog(int64_t maxBytes = 10 * 1024 * 1024, int keep = 5);
    ~RotatingLog();
    RotatingLog(const RotatingLog &) = delete;
    RotatingLog &operator=(const RotatingLog &) = delete;
    bool open(const std::string &path);
    bool isOpen() const {
        return file_ != nullptr;
    }
    // One line, without its end of line, handed to the system at once: a log that is read while it
    // is written, or after a crash, has what was said.
    void write(const std::string &line);

  private:
    std::FILE *file_ = nullptr;
    std::string path_;
    int64_t maxBytes_, size_ = 0;
    int keep_;
    void rotate();
};
} // namespace tools
