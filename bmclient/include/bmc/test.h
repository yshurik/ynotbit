// A small test harness with no dependency: tests register themselves when the program starts and
// tests/bmclient/main.cpp runs them.
#pragma once
#include <bmc/bytes.h>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace bmc::test {
struct Case {
    const char *name;
    std::function<void()> run;
};
inline std::vector<Case> &cases() {
    static std::vector<Case> list;
    return list;
}
struct Registrar {
    Registrar(const char *name, std::function<void()> run) {
        cases().push_back({name, std::move(run)});
    }
};
struct Failure {
    std::string message;
};
[[noreturn]] inline void fail(const char *file, int line, const std::string &what) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + what};
}
template <class T> std::string show(const T &value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
        return "\"" + std::string(value) + "\"";
    } else if constexpr (std::is_same_v<T, bmc::Bytes>) {
        return "bytes[" + std::to_string(value.size()) + "] " + bmc::hex(value);
    } else if constexpr (requires(std::ostream &out) { out << value; }) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else {
        return "<value>";
    }
}
} // namespace bmc::test

#define BMC_TEST_JOIN2(a, b) a##b
#define BMC_TEST_JOIN(a, b) BMC_TEST_JOIN2(a, b)
#define TEST(name)                                                                                 \
    static void BMC_TEST_JOIN(bmc_test_, name)();                                                  \
    static ::bmc::test::Registrar BMC_TEST_JOIN(bmc_registrar_,                                    \
                                                name)(#name, BMC_TEST_JOIN(bmc_test_, name));      \
    static void BMC_TEST_JOIN(bmc_test_, name)()
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond))                                                                               \
            ::bmc::test::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed");                      \
    } while (0)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto bmc_a = (a);                                                                    \
        const auto bmc_b = (b);                                                                    \
        if (!(bmc_a == bmc_b))                                                                     \
            ::bmc::test::fail(__FILE__, __LINE__,                                                  \
                              std::string("CHECK_EQ(" #a ", " #b ") failed: ") +                   \
                                  ::bmc::test::show(bmc_a) + " != " + ::bmc::test::show(bmc_b));   \
    } while (0)
#define CHECK_THROWS(expr)                                                                         \
    do {                                                                                           \
        bool bmc_threw = false;                                                                    \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (const std::exception &) {                                                         \
            bmc_threw = true;                                                                      \
        }                                                                                          \
        if (!bmc_threw)                                                                            \
            ::bmc::test::fail(__FILE__, __LINE__, "CHECK_THROWS(" #expr ") did not throw");        \
    } while (0)
