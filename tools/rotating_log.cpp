#include "rotating_log.h"
#include <sys/stat.h>

namespace tools {
RotatingLog::RotatingLog(int64_t maxBytes, int keep) : maxBytes_(maxBytes), keep_(keep) {}

RotatingLog::~RotatingLog() {
    if (file_)
        std::fclose(file_);
}

bool RotatingLog::open(const std::string &path) {
    if (file_)
        std::fclose(file_);
    path_ = path;
    file_ = std::fopen(path.c_str(), "ab");
    size_ = 0;
    if (file_) {
        struct stat info;
        if (::stat(path.c_str(), &info) == 0)
            size_ = int64_t(info.st_size);
    }
    return file_ != nullptr;
}

void RotatingLog::write(const std::string &line) {
    if (!file_)
        return;
    const int64_t bytes = int64_t(line.size()) + 1;
    if (size_ > 0 && size_ + bytes > maxBytes_)
        rotate();
    if (!file_)
        return;
    std::fwrite(line.data(), 1, line.size(), file_);
    std::fputc('\n', file_);
    std::fflush(file_);
    size_ += bytes;
}

// log.N goes, log.N-1 becomes log.N, ... and the full log becomes log.1.
void RotatingLog::rotate() {
    std::fclose(file_);
    file_ = nullptr;
    std::remove((path_ + "." + std::to_string(keep_)).c_str());
    for (int i = keep_ - 1; i >= 1; --i)
        std::rename((path_ + "." + std::to_string(i)).c_str(),
                    (path_ + "." + std::to_string(i + 1)).c_str());
    std::rename(path_.c_str(), (path_ + ".1").c_str());
    file_ = std::fopen(path_.c_str(), "ab");
    size_ = 0;
}
} // namespace tools
