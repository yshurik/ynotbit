// A child for the supervisor's tests on Windows, which has no shell scripts to write: what it does
// is in the environment variable NODE_CHILD, actions separated by ';' and done in order. The
// supervisor gives it the arguments of a node (--node -D ...), which it ignores.
//
//   exit=N          ends with that exit code
//   crash           ends as the system ends a program: an NTSTATUS for an exit code
//   err=TEXT        a line on stderr          out=TEXT   a line on stdout
//   outspam=N       N lines on stdout, more than a pipe holds when N is 4000
//   sleep=MS        waits                     wait       waits for ever
//   parentpid=FILE  writes the YNOTBIT_PARENT_PID it was given to FILE
//   grandchild=FILE starts a child of its own that waits for ever, and writes its pid to FILE
#ifdef _WIN32
#define NOMINMAX
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <windows.h>

namespace {
std::string environment(const char *name) {
    char buffer[4096];
    const DWORD length = GetEnvironmentVariableA(name, buffer, DWORD(sizeof buffer));
    return length && length < sizeof buffer ? std::string(buffer, length) : std::string();
}
void sleepFor(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
void writeTo(const std::string &path, const std::string &text) {
    if (std::FILE *file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}
} // namespace

int main() {
    std::string rest = environment("NODE_CHILD");
    while (!rest.empty()) {
        const auto cut = rest.find(';');
        const std::string action = rest.substr(0, cut);
        rest = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
        const auto equal = action.find('=');
        const std::string name = action.substr(0, equal);
        const std::string value =
            equal == std::string::npos ? std::string() : action.substr(equal + 1);
        if (name == "exit") {
            return std::atoi(value.c_str());
        } else if (name == "crash") {
            TerminateProcess(GetCurrentProcess(), 0xC0000005u);
        } else if (name == "err") {
            std::fprintf(stderr, "%s\n", value.c_str());
            std::fflush(stderr);
        } else if (name == "out") {
            std::fprintf(stdout, "%s\n", value.c_str());
            std::fflush(stdout);
        } else if (name == "outspam") {
            for (int i = 0, n = std::atoi(value.c_str()); i < n; ++i)
                std::fputs("[2026] a line of the node log, long enough to fill a pipe\n", stdout);
            std::fflush(stdout);
        } else if (name == "sleep") {
            sleepFor(std::atoi(value.c_str()));
        } else if (name == "wait") {
            for (;;)
                sleepFor(1000);
        } else if (name == "parentpid") {
            writeTo(value, environment("YNOTBIT_PARENT_PID"));
        } else if (name == "grandchild") {
            char self[MAX_PATH];
            GetModuleFileNameA(nullptr, self, MAX_PATH);
            SetEnvironmentVariableA("NODE_CHILD", "wait");
            STARTUPINFOA start{};
            start.cb = sizeof start;
            PROCESS_INFORMATION process{};
            std::string commandLine = std::string("\"") + self + "\"";
            if (CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &start, &process)) {
                writeTo(value, std::to_string(process.dwProcessId));
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }
    }
    return 0;
}
#else
int main() {
    return 0;
}
#endif
