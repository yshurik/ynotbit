// The file helpers the vault and the mailbox sit on: the atomic write that has to leave either the
// old bytes or the new ones, the exclusive create that must not step on a name, and what both say
// when the filesystem says no.
#include <bmc/error.h>
#include <bmc/path.h>
#include <bmc/test.h>
#include <filesystem>

namespace {
size_t tempFilesIn(const std::filesystem::path &dir) {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
        if (entry.path().filename().string().find(".tmp-") != std::string::npos)
            ++count;
    return count;
}
} // namespace

TEST(write_file_atomic_leaves_the_target_or_the_old_bytes) {
    const auto dir = std::filesystem::path(bmc::test::tempDir());
    const auto target = dir / "file.bin";
    bmc::writeFileAtomic(target, bmc::view(std::string("first")));
    CHECK_EQ(bmc::stringOf(bmc::test::readFile(target.string())), std::string("first"));
    bmc::writeFileAtomic(target, bmc::view(std::string("second"))); // over what is there
    CHECK_EQ(bmc::stringOf(bmc::test::readFile(target.string())), std::string("second"));
    bmc::writeFileAtomic(target, bmc::ByteView()); // empty is a write like any other
    CHECK_EQ(std::filesystem::file_size(target), uintmax_t(0));
    CHECK_EQ(tempFilesIn(dir), size_t(0)); // and no temp file is left behind
    // A target the rename cannot replace: a directory that is not empty. The write fails, the
    // directory is left as it was, and the temp file goes away with it.
    const auto busy = dir / "busy";
    std::filesystem::create_directory(busy);
    bmc::test::writeFile((busy / "inside").string(), bmc::view(std::string("x")));
    try {
        bmc::writeFileAtomic(busy, bmc::view(std::string("x")));
        bmc::test::fail(__FILE__, __LINE__, "a write over a directory looked like a success");
    } catch (const bmc::Error &e) {
        CHECK_EQ(int(e.code()), int(bmc::Code::Io));
    }
    CHECK(std::filesystem::exists(busy / "inside"));
    CHECK_EQ(tempFilesIn(dir), size_t(0));
    // And a target in a directory that is not there at all.
    CHECK_THROWS(
        bmc::writeFileAtomic(dir / "no" / "such" / "file.bin", bmc::view(std::string("x"))));
    CHECK_EQ(tempFilesIn(dir), size_t(0));
}

TEST(reserve_file_takes_a_name_or_refuses_it) {
    const auto dir = std::filesystem::path(bmc::test::tempDir());
    const auto target = dir / "new.bmmail";
    bmc::reserveFile(target);
    CHECK(std::filesystem::exists(target));
    CHECK_EQ(std::filesystem::file_size(target), uintmax_t(0));
    // Owner read and write and nothing for anyone else -- where the platform reports permission
    // bits at all, which is asked of a probe file rather than assumed (as the vault's tests do).
    const auto probe = dir / "probe";
    bmc::test::writeFile(probe.string(), bmc::view(std::string("x")));
    std::filesystem::permissions(probe, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace);
    const bool bitsBind =
        std::filesystem::status(probe).permissions() == std::filesystem::perms::owner_read;
    if (bitsBind)
        CHECK_EQ(std::filesystem::status(target).permissions(),
                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    // The name is taken once: a second reserve of the same path is refused by name, and so is a
    // path under a directory that is not there.
    try {
        bmc::reserveFile(target);
        bmc::test::fail(__FILE__, __LINE__, "a name that was taken was reserved twice");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("File already exists or cannot be created"));
    }
    CHECK_EQ(std::filesystem::file_size(target), uintmax_t(0)); // and it was not touched
    CHECK_THROWS(bmc::reserveFile(dir / "no" / "such" / "new.bmmail"));
}
