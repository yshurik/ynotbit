#include <bmc/path.h>
#include <bmc/random.h>
#include <cstdio>
#include <system_error>

namespace bmc {
std::filesystem::path path(const std::string &utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

void writeFileAtomic(const std::filesystem::path &target, ByteView bytes) {
    auto temp = target;
    temp += ".tmp-" + uuid();
    std::FILE *f = std::fopen(temp.string().c_str(), "wb");
    if (!f)
        throw Error(Code::Io, "Cannot write vault");
    const bool wrote =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool flushed = std::fflush(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !flushed || !closed) {
        std::filesystem::remove(temp);
        throw Error(Code::Io, "Cannot write vault");
    }
    std::error_code ec;
    std::filesystem::permissions(
        temp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp);
        throw Error(Code::Io, "Vault save failed; previous vault preserved");
    }
}

void reserveFile(const std::filesystem::path &target) {
    // The C11 'x' mode: fail if the file is there. glibc and MSVC both have it.
    std::FILE *f = std::fopen(target.string().c_str(), "wbx");
    if (!f)
        throw Error(Code::Io, "File already exists or cannot be created");
    std::fclose(f);
    std::error_code ec;
    std::filesystem::permissions(
        target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec)
        throw Error(Code::Io, "Cannot protect file permissions");
}
} // namespace bmc
