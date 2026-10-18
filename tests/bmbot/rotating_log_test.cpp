// A log that is kept for months keeps to a size: when the next line would pass it, the log becomes
// log.1, log.1 becomes log.2, and the oldest of the kept ones goes.
#include "rotating_log.h"
#include <bmc/test.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
std::string read(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
bool exists(const std::string &path) {
    return std::filesystem::exists(path);
}
} // namespace

TEST(a_log_keeps_to_its_size_and_a_few_rotated_files) {
    const auto path = bmc::test::tempDir() + "/service.log";
    tools::RotatingLog rotating(200, 3);
    CHECK(rotating.open(path));
    for (int i = 0; i < 40; ++i) {
        char line[80];
        std::snprintf(line, sizeof line, "line %3d of the log, padded to twenty", i);
        rotating.write(line);
    }
    CHECK(exists(path) && exists(path + ".1") && exists(path + ".2") && exists(path + ".3"));
    CHECK(!exists(path + ".4")); // the log and the three kept before it, and no more
    const auto text = read(path);
    CHECK(text.find("line  39 ") != std::string::npos);
    CHECK(text.find("line   0 ") == std::string::npos); // the oldest lines are gone
    for (const auto *suffix : {"", ".1", ".2", ".3"})
        CHECK(std::filesystem::file_size(path + suffix) <= 200 + 60); // never past it by a line
    tools::RotatingLog again(200, 3);
    CHECK(again.open(path));
    again.write("after a restart");
    // It goes on where it was: the new line follows the old ones, in the log or, if that was
    // nearly full, in the log after it was rotated.
    CHECK(read(path).find("after a restart") != std::string::npos);
    CHECK((read(path) + read(path + ".1")).find("line  39 ") != std::string::npos);
    CHECK(!exists(path + ".4"));
}

TEST(a_log_that_cannot_be_opened_says_so_and_swallows_lines) {
    tools::RotatingLog log;
    CHECK(!log.open("/nonexistent-directory/service.log"));
    log.write("nobody hears this"); // no crash
    CHECK(!log.isOpen());
}
