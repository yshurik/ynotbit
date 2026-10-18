// Runs the tests registered by bmc/test.h: `bmclient_tests [--list] [--filter text]`.
#include <bmc/clock.h>
#include <bmc/test.h>
#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, char **argv) {
    const char *filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) {
            for (const auto &c : bmc::test::cases())
                std::puts(c.name);
            return 0;
        }
        if (!std::strcmp(argv[i], "--filter") && i + 1 < argc)
            filter = argv[++i];
    }
    int ran = 0, failed = 0;
    for (const auto &c : bmc::test::cases()) {
        if (filter && !std::strstr(c.name, filter))
            continue;
        ++ran;
        try {
            c.run();
            std::printf("PASS %s\n", c.name);
        } catch (const bmc::test::Failure &f) {
            ++failed;
            // A test that failed part way through may have left a clock of its own behind, and
            // every test after it would run on that clock.
            bmc::setClockForTesting(nullptr);
            std::printf("FAIL %s: %s\n", c.name, f.message.c_str());
        } catch (const std::exception &e) {
            ++failed;
            bmc::setClockForTesting(nullptr);
            std::printf("FAIL %s: unexpected exception: %s\n", c.name, e.what());
        }
        std::fflush(stdout);
    }
    std::printf("%d run, %d failed\n", ran, failed);
    return ran == 0 || failed ? 1 : 0;
}
