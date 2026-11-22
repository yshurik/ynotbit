// Runs the tests registered by bmc/test.h: `bmclient_tests [--list] [--filter text]`.
#include <bmc/clock.h>
#include <bmc/test.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

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
        const auto started = std::chrono::steady_clock::now();
        // A test that takes seconds says so: a search for a nonce is random, and a runner that is
        // slow shows it here before it shows it as a deadline.
        const auto took = [&] {
            const auto seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            char text[32] = "";
            if (seconds >= 5)
                std::snprintf(text, sizeof text, " (%.1f s)", seconds);
            return std::string(text);
        };
        try {
            c.run();
            std::printf("PASS %s%s\n", c.name, took().c_str());
        } catch (const bmc::test::Failure &f) {
            ++failed;
            // A test that failed part way through may have left a clock of its own behind, and
            // every test after it would run on that clock.
            bmc::setClockForTesting(nullptr);
            std::printf("FAIL %s%s: %s\n", c.name, took().c_str(), f.message.c_str());
        } catch (const std::exception &e) {
            ++failed;
            bmc::setClockForTesting(nullptr);
            std::printf("FAIL %s%s: unexpected exception: %s\n", c.name, took().c_str(), e.what());
        }
        std::fflush(stdout);
    }
    std::printf("%d run, %d failed\n", ran, failed);
    return ran == 0 || failed ? 1 : 0;
}
