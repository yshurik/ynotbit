// Paths and files, the way the storage layer needs them: a UTF-8 string to a std path, an atomic
// write (what QSaveFile did), and an exclusive create with owner-only permissions (what
// QFile::NewOnly plus setPermissions did). No POSIX-only call before the last slice's supervisor.
#pragma once
#include <bmc/bytes.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace bmc {
// A UTF-8 path as the filesystem sees it. On POSIX the bytes as they are (QFile::encodeName did the
// same); on Windows a wide path.
std::filesystem::path path(const std::string &utf8);

// The C FILE behind a path, opened the way the platform wants it: the wide path on Windows, so a
// directory outside the ANSI code page works, and the bytes as they are on POSIX. Null when it
// cannot be opened. The caller closes it.
std::FILE *openFile(const std::filesystem::path &p, const char *mode);

// Write the bytes to a temp file beside the target, flush, and rename it over the target, so a
// reader sees either the old file or the new one. Throws Error(Io) on any failure; the target is
// left as it was.
void writeFileAtomic(const std::filesystem::path &target, ByteView bytes);

// Create the file, owner read and write only, or throw Error(Io, "File already exists or cannot be
// created"). The file is empty and left open by nobody.
void reserveFile(const std::filesystem::path &target);
} // namespace bmc
