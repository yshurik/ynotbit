#include <bmc/path.h>
#include <bmc/random.h>
#include <cstdio>
#include <cstring>
#include <system_error>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

namespace bmc {
std::filesystem::path path(const std::string &utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::FILE *openFile(const std::filesystem::path &p, const char *mode) {
#ifdef _WIN32
    // The wide path the filesystem gave us. std::fopen would take the ANSI code page instead and
    // lose every character outside it, so a vault in a directory with a non-ASCII name in it could
    // not be opened at all.
    const std::wstring wide(mode, mode + std::strlen(mode));
    return ::_wfopen(p.c_str(), wide.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

void writeFileAtomic(const std::filesystem::path &target, ByteView bytes) {
    auto temp = target;
    temp += ".tmp-" + uuid();
    std::FILE *f = openFile(temp, "wb");
    if (!f)
        throw Error(Code::Io, "Cannot write vault");
    const bool wrote =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool flushed = std::fflush(f) == 0;
    // To the disk before the rename publishes it: the write is what a reader may find under the
    // name of the vault, and the release this replaces committed with the same flush-and-sync.
#ifdef _WIN32
    const bool synced = ::_commit(::_fileno(f)) == 0;
#else
    const bool synced = ::fsync(::fileno(f)) == 0;
#endif
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !flushed || !synced || !closed) {
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
    std::FILE *f = openFile(target, "wbx");
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
