// The node process on Windows: CreateProcess into a job object, a pipe for what it says, and
// TerminateJobObject to stop the whole group. There are no signals, so a stop is a kill — which
// is what QProcess::terminate did to this windowless child — and "crashed" means the app stopped
// it, as QProcess called that a CrashExit.
//
// This unit is compiled on Windows only; POSIX builds get node_supervisor_posix.cpp.
#ifdef _WIN32
#include <bmc/node_supervisor.h>
#define NOMINMAX
#include <string>
#include <vector>
#include <windows.h>

namespace bmc {
namespace {
std::string trimmed(const std::string &text) {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    size_t first = 0, last = text.size();
    while (first < last && space(text[first]))
        ++first;
    while (last > first && space(text[last - 1]))
        --last;
    return text.substr(first, last - first);
}

std::string lastErrorText() {
    const DWORD code = GetLastError();
    char *message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char *>(&message), 0, nullptr);
    std::string text = length && message ? std::string(message, size_t(length))
                                         : "error " + std::to_string(int64_t(code));
    if (message)
        LocalFree(message);
    return trimmed(text);
}

std::wstring wide(const std::string &utf8) {
    if (utf8.empty())
        return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), length);
    return out;
}

std::wstring quoted(const std::wstring &argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
        return argument;
    std::wstring out = L"\"";
    for (const wchar_t c : argument) {
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    return out + L"\"";
}
} // namespace

struct NodeSupervisor::Impl {
    explicit Impl(NodeConfig config) : config(std::move(config)) {
        lastError = "not started";
    }
    ~Impl() {
        close();
    }

    // Waits not at all: the child is gone when it says so. True when it is gone now.
    bool reap() {
        if (!process)
            return true;
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
            return false;
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        exitCode = int(code);
        exited = true;
        crashed = stopRequested;
        CloseHandle(process);
        process = nullptr;
        if (job) {
            CloseHandle(job); // the job's kill-on-close has nothing left to kill
            job = nullptr;
        }
        close();
        return true;
    }
    void close() {
        if (readEnd) {
            CloseHandle(readEnd);
            readEnd = nullptr;
        }
    }
    void readWhatItSaid() {
        if (!readEnd)
            return;
        std::string arrived;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) || available == 0)
                break;
            char buffer[4096];
            const DWORD wanted = available < sizeof buffer ? available : DWORD(sizeof buffer);
            DWORD got = 0;
            if (!ReadFile(readEnd, buffer, wanted, &got, nullptr) || got == 0)
                break;
            arrived.append(buffer, size_t(got));
        }
        const std::string text = trimmed(arrived);
        if (text.empty())
            return;
        chunk = text.substr(0, 400);
        const auto newline = text.rfind('\n');
        line = text.substr(newline == std::string::npos ? 0 : newline + 1).substr(0, 200);
    }

    NodeConfig config;
    HANDLE process = nullptr, job = nullptr, readEnd = nullptr;
    int exitCode = 0;
    bool crashed = false, exited = false, stopRequested = false;
    std::string lastError, line, chunk;
};

NodeSupervisor::NodeSupervisor(NodeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
NodeSupervisor::~NodeSupervisor() {
    stop();
}

bool NodeSupervisor::start() {
    if (!impl_->reap())
        return true; // still running: nothing to do, as Session never asks twice
    impl_->close();
    impl_->exited = impl_->crashed = impl_->stopRequested = false;
    impl_->exitCode = 0;
    impl_->lastError.clear();
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        impl_->lastError = lastErrorText();
        return false;
    }
    // A job object of its own, and one that dies with the app: the child and whatever it forks.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    auto arguments = impl_->config.nodeArguments();
    std::vector<std::wstring> parts;
    parts.push_back(quoted(wide(impl_->config.bin)));
    for (const auto &argument : arguments)
        parts.push_back(quoted(wide(argument)));
    std::wstring commandLine;
    for (const auto &part : parts) {
        if (!commandLine.empty())
            commandLine += L' ';
        commandLine += part;
    }
    // The inherited environment plus the parent pid the relay bridge checks.
    std::wstring environment;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (const wchar_t *it = block; *it; it += std::wcslen(it) + 1)
            if (std::wcsncmp(it, L"YNOTBIT_PARENT_PID=", 19) != 0)
                environment.append(it, std::wcslen(it) + 1);
        FreeEnvironmentStringsW(block);
    }
    environment += L"YNOTBIT_PARENT_PID=" + std::to_wstring(int64_t(GetCurrentProcessId())) + L'\0';
    environment += L'\0';
    STARTUPINFOW start{};
    start.cb = sizeof start;
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdOutput = writeEnd; // read and dropped, as Session did
    start.hStdError = writeEnd;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                                        environment.data(), nullptr, &start, &process);
    const std::string failure = started ? std::string() : lastErrorText();
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        if (job)
            CloseHandle(job);
        impl_->lastError = failure;
        return false;
    }
    CloseHandle(process.hThread);
    impl_->process = process.hProcess;
    impl_->readEnd = readEnd;
    if (job) {
        if (!AssignProcessToJobObject(job, impl_->process)) {
            CloseHandle(job); // the app is already in a job of its own: the child runs unhooked
            job = nullptr;
        }
    }
    impl_->job = job;
    return true;
}

bool NodeSupervisor::poll() {
    impl_->readWhatItSaid();
    const bool ran = impl_->process != nullptr;
    return impl_->reap() && ran;
}

void NodeSupervisor::stop() {
    impl_->readWhatItSaid();
    if (impl_->reap())
        return;
    impl_->stopRequested = true;
    if (impl_->job)
        TerminateJobObject(impl_->job, 1);
    else
        TerminateProcess(impl_->process, 1);
    WaitForSingleObject(impl_->process, 3000);
    impl_->reap();
    impl_->readWhatItSaid(); // whatever it said on the way out
}

bool NodeSupervisor::restart() {
    stop();
    return start();
}

bool NodeSupervisor::running() const {
    return impl_->process != nullptr && !impl_->exited;
}
int64_t NodeSupervisor::pid() const {
    return impl_->process ? int64_t(GetProcessId(impl_->process)) : 0;
}
int NodeSupervisor::exitCode() const {
    return impl_->exitCode;
}
bool NodeSupervisor::crashed() const {
    return impl_->crashed;
}
bool NodeSupervisor::exited() const {
    return impl_->exited;
}
const std::string &NodeSupervisor::lastError() const {
    return impl_->lastError;
}
const std::string &NodeSupervisor::standardError() const {
    return impl_->line;
}
const std::string &NodeSupervisor::standardErrorChunk() const {
    return impl_->chunk;
}
const NodeConfig &NodeSupervisor::config() const {
    return impl_->config;
}
} // namespace bmc
#endif // _WIN32
