// The node process on POSIX: posix_spawn into a process group of its own, a non-blocking pipe for
// what it says, waitpid to reap it, and kill(2) at the group to stop it.
#ifndef _WIN32
#include <bmc/node_supervisor.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace bmc {
namespace {
// The app's trimmed() and its "last line" read: QProcess handed Session whatever had arrived.
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
bool groupIsGone(pid_t group) {
    return ::kill(-group, 0) != 0 && errno == ESRCH;
}
void sleepMillis(int millis) {
    timespec wanted{millis / 1000, (millis % 1000) * 1000000L};
    while (nanosleep(&wanted, &wanted) == -1 && errno == EINTR) {
    }
}
} // namespace

struct NodeSupervisor::Impl {
    explicit Impl(NodeConfig config) : config(std::move(config)) {
        lastError = "not started";
    }
    ~Impl() {
        close();
    }

    // Reaps the child when it is already gone. True when it is gone now.
    bool reap() {
        if (pid <= 0)
            return true;
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done != pid)
            return false;
        pid = 0;
        exited = true;
        crashed = WIFSIGNALED(status);
        exitCode = crashed ? 0 : WIFEXITED(status) ? WEXITSTATUS(status) : 0;
        return true;
    }
    void closeOne(int &descriptor) {
        if (descriptor >= 0) {
            ::close(descriptor);
            descriptor = -1;
        }
    }
    void close() {
        closeOne(fd);
        closeOne(outFd);
    }
    // The node logs to stdout, and the app dropped every word of it (QProcess kept the two
    // channels apart, and Session read that one to throw it away). It is still read: a pipe nobody
    // drains fills up and the node would block on its own log.
    void discardWhatItLogged() {
        char buffer[4096];
        while (outFd >= 0) {
            const ssize_t got = ::read(outFd, buffer, sizeof buffer);
            if (got > 0)
                continue;
            if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                closeOne(outFd);
            break;
        }
    }
    void readWhatItSaid() {
        discardWhatItLogged();
        if (fd < 0)
            return;
        std::string arrived;
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(fd, buffer, sizeof buffer);
            if (got > 0) {
                arrived.append(buffer, size_t(got));
                continue;
            }
            if (got == 0) // the child closed it, or exited
                closeOne(fd);
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                closeOne(fd);
            break;
        }
        const std::string text = trimmed(arrived);
        if (text.empty())
            return;
        chunk = text.substr(0, 400);
        const auto newline = text.rfind('\n');
        line = text.substr(newline == std::string::npos ? 0 : newline + 1).substr(0, 200);
    }

    NodeConfig config;
    pid_t pid = 0;
    int fd = -1;    // the child's stderr: what the app shows
    int outFd = -1; // the child's stdout: its log, dropped
    int exitCode = 0;
    bool crashed = false, exited = false;
    std::string lastError, line, chunk;
};

NodeSupervisor::NodeSupervisor(NodeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
NodeSupervisor::~NodeSupervisor() {
    // The app stops the node itself before it goes; this is for the paths that do not.
    stop();
}

bool NodeSupervisor::start() {
    if (!impl_->reap())
        return true; // still running: nothing to do, as Session never asks twice
    impl_->close();
    impl_->exited = impl_->crashed = false;
    impl_->exitCode = 0;
    impl_->lastError.clear();
    impl_->line.clear(); // the dead child's last words are not the new child's
    impl_->chunk.clear();
    int ends[2];    // the child's stderr: what the app shows
    int outEnds[2]; // the child's stdout: the node's log, read and dropped
    if (::pipe(ends) != 0) {
        impl_->lastError = std::strerror(errno);
        return false;
    }
    if (::pipe(outEnds) != 0) {
        impl_->lastError = std::strerror(errno);
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    // Two pipes, as QProcess had two channels. On one, the node's log would arrive as its errors.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outEnds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    posix_spawn_file_actions_addclose(&actions, outEnds[0]);
    posix_spawn_file_actions_addclose(&actions, outEnds[1]);
    // A group of its own, so stopping the node stops whatever the node forked.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    // The inherited environment plus the parent pid the relay bridge checks.
    std::vector<std::string> environment;
    for (char **it = environ; it && *it; ++it)
        if (std::strncmp(*it, "YNOTBIT_PARENT_PID=", 19) != 0)
            environment.emplace_back(*it);
    environment.emplace_back("YNOTBIT_PARENT_PID=" + std::to_string(int64_t(getpid())));
    auto arguments = impl_->config.nodeArguments();
    std::vector<char *> argv, envp;
    argv.push_back(const_cast<char *>(impl_->config.bin.c_str()));
    for (auto &argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    for (auto &variable : environment)
        envp.push_back(variable.data());
    envp.push_back(nullptr);
    pid_t child = 0;
    const int failure = posix_spawn(&child, impl_->config.bin.c_str(), &actions, &attributes,
                                    argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(ends[1]);
    ::close(outEnds[1]);
    if (failure != 0) {
        ::close(ends[0]);
        ::close(outEnds[0]);
        impl_->lastError = std::strerror(failure);
        return false;
    }
    impl_->pid = child;
    impl_->fd = ends[0];
    impl_->outFd = outEnds[0];
    for (int *descriptor : {&impl_->fd, &impl_->outFd}) {
        const int flags = fcntl(*descriptor, F_GETFL, 0);
        if (flags >= 0)
            fcntl(*descriptor, F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

bool NodeSupervisor::poll() {
    impl_->readWhatItSaid();
    const bool ran = impl_->pid > 0;
    // reap() reaps it, and says whether it is gone; an exit that never happened is not news.
    return impl_->reap() && ran;
}

void NodeSupervisor::stop() {
    // The group's id is the child's pid, and it is still that id after the child itself is reaped:
    // a process the node forked may outlive it (which is why the stop goes to the whole group).
    // Reaping first is what makes this a call on a freed pid: another process could take that pid
    // between the reap and the kill. The window is one kill wide, and the alternative (a pidfd, or
    // never reaping until after the kill) buys less than it costs; the app stops the node it owns.
    const pid_t child = impl_->pid;
    impl_->readWhatItSaid();
    impl_->reap();
    if (child > 0)
        ::kill(-child, SIGTERM);
    // The three seconds are the clock's, not sixty sleeps of 50 ms: a process the system throttles
    // gets its timers late (a sleep of 50 ms came back after 135), and sixty of those took eight
    // seconds and more.
    const auto leaveBy = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (true) {
        impl_->readWhatItSaid(); // it may be logging its way out, and an unread pipe stops it
        impl_->reap();
        if (impl_->pid <= 0 && (child <= 0 || groupIsGone(child)))
            break;
        if (std::chrono::steady_clock::now() >= leaveBy)
            break;
        sleepMillis(50);
    }
    // Anything of the group still there after its three seconds is killed; a group that is gone
    // makes this a failed call and nothing else.
    if (child > 0 && !groupIsGone(child))
        ::kill(-child, SIGKILL);
    const auto reapBy = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (impl_->pid > 0 && !impl_->reap() && std::chrono::steady_clock::now() < reapBy) {
        impl_->readWhatItSaid();
        sleepMillis(50);
    }
    impl_->readWhatItSaid(); // whatever it said on the way out
    impl_->close();
}

bool NodeSupervisor::restart() {
    stop();
    return start();
}

bool NodeSupervisor::running() const {
    return impl_->pid > 0;
}
int64_t NodeSupervisor::pid() const {
    return impl_->pid;
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
#endif // !_WIN32
