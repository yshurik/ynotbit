// The node process: started, noticed when it goes, asked what it said, and stopped. The children
// are shell scripts of the test's own on POSIX and node_child.exe on Windows, which has none: no
// node binary, no port and no network are involved. The last test of the first part starts the real
// node, bmnode, on every system, with no peer but a closed port.
#include <bmc/node_supervisor.h>
#include <bmc/test.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

namespace {
// A script that ignores the node's own argv (the supervisor passes it whatever the config makes).
std::string scriptFor(const std::string &body) {
    const auto file = std::filesystem::path(bmc::test::tempDir()) / "child.sh";
    std::ofstream out(file);
    out << "#!/bin/sh\n" << body << "\n";
    out.close();
    std::filesystem::permissions(file, std::filesystem::perms::owner_all |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::others_read);
    return file.string();
}

struct Child {
    bmc::NodeConfig config;
    explicit Child(const std::string &body) {
        config.bin = scriptFor(body);
        config.root = bmc::test::tempDir();
        config.maildir = config.root + "/unused-maildir";
    }
};

// The caller polls once a tick; the tests wait for the news the way it arrives.
bool until(const std::function<bool()> &done) {
    for (int waited = 0; waited < 5000; waited += 10) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// Polls the way the caller does, until the child has said what the test waits for.
bool untilSaid(bmc::NodeSupervisor &node, const std::string &text) {
    return until([&] {
        node.poll();
        return node.standardError() == text;
    });
}

std::string fileText(const std::string &path) {
    return bmc::stringOf(bmc::test::readFile(path));
}

// macOS gives a process it throttles its timers late: with this on, a sleep of 50 ms comes back
// after about 135 (measured), which is how `taskpolicy -b` runs a program. Other systems have no
// such thing here, and a test that calls this runs there without the throttle.
void throttled(bool on) {
#ifdef __APPLE__
    setpriority(PRIO_DARWIN_PROCESS, 0, on ? PRIO_DARWIN_BG : 0);
#else
    (void)on;
#endif
}
} // namespace

TEST(a_child_that_exits_is_noticed) {
    Child child("exit 3");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(node.running());
    CHECK(node.pid() > 0);
    CHECK(node.lastError().empty());
    CHECK(!node.exited());
    CHECK(until([&] { return node.poll(); })); // the first poll after the exit is the news
    CHECK(!node.running());
    CHECK_EQ(node.pid(), int64_t(0));
    CHECK(node.exited());
    CHECK(!node.crashed());
    CHECK_EQ(node.exitCode(), 3);
    CHECK(!node.poll()); // an exit is news once, not every tick
}

TEST(a_child_that_is_killed_is_called_a_crash) {
    Child child("kill -9 $$");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(until([&] { return node.poll(); }));
    CHECK(node.exited());
    CHECK(node.crashed());
    CHECK_EQ(node.exitCode(), 0);
}

TEST(what_it_wrote_to_stderr_is_kept) {
    Child child("echo one >&2; echo two >&2; exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "two"));
    CHECK_EQ(node.standardError(), std::string("two")); // the last line: what the node stopped of
    CHECK_EQ(node.standardErrorChunk(), std::string("one\ntwo")); // and the chunk it came in
    node.stop();
    CHECK(!node.running());
}

// The node logs to stdout -- ntb-daemon.c sends its log there -- and the app never showed a word
// of it. On one pipe for both, that log would arrive as "the node said": the complaint here comes
// first and the log line last, so a merged stream would answer with the wrong one.
TEST(what_it_logs_is_not_what_it_says) {
    Child child("echo 'cannot bind the port' >&2; echo '[2026] loading saved object store'; "
                "exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "cannot bind the port"));
    CHECK_EQ(node.standardError(), std::string("cannot bind the port"));
    CHECK_EQ(node.standardErrorChunk(), std::string("cannot bind the port"));
    node.stop();
    CHECK(!node.running());
}

// And the log is still read: a pipe nobody drains fills up, and the node would block on a line of
// its own log. 200 KB is well past the 64 KB a pipe holds, so a child that got through this and
// reached its last line was drained.
TEST(what_it_logs_is_drained_even_though_it_is_dropped) {
    Child child("i=0; while [ $i -lt 4000 ]; do "
                "echo '[2026] a line of the node log, long enough to fill a pipe'; "
                "i=$((i+1)); done; echo 'done logging' >&2; exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "done logging"));
    CHECK_EQ(node.standardError(), std::string("done logging"));
    node.stop();
    CHECK(!node.running());
}

TEST(a_restart_starts_it_again_and_the_stop_is_not_news) {
    Child child("exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    const int64_t first = node.pid();
    CHECK(first > 0);
    CHECK(node.restart());
    CHECK(node.running());
    CHECK(node.pid() > 0);
    CHECK(node.pid() != first);
    CHECK(!node.poll()); // a stop this side asked for is not an exit to report
    node.stop();
    CHECK(!node.running());
}

// And the new child starts with nothing said: Session::startNode() clears what it shows, and the
// supervisor has to clear what it read, or the dead child's last line is read as the new one's.
TEST(a_restart_forgets_what_the_last_one_said) {
    // Complains once, in a file beside itself, and says nothing the second time it runs.
    Child child("if [ ! -f \"$0.said\" ]; then echo 'the first child complained' >&2; "
                "touch \"$0.said\"; fi; exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "the first child complained"));
    CHECK(node.restart());
    CHECK(node.standardError().empty());
    CHECK(node.standardErrorChunk().empty());
    node.stop();
    CHECK(!node.running());
}

// A node that has a lot to say on the way out is still allowed to say it: while stop() waits for
// it, the pipe it writes into is read, as QProcess::waitForFinished() read its channels. A node
// blocked on a full pipe would never exit and would be killed instead of shut down. The child says
// "ready" on stderr first, so the stop cannot arrive before its trap is installed -- which is a
// race this test lost once already.
TEST(a_child_that_logs_its_way_out_is_not_killed_for_it) {
    Child child("trap 'i=0; while [ $i -lt 4000 ]; do "
                "echo \"a line of the node log, long enough to fill a pipe\"; i=$((i+1)); "
                "done; echo came-out-fine >&2; exit 0' TERM; echo ready >&2; sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "ready"));
    node.stop();
    CHECK(!node.running());
    CHECK(!node.crashed()); // SIGKILL would show here: it finished on its own terms
    CHECK_EQ(node.standardError(), std::string("came-out-fine"));
    CHECK_EQ(node.exitCode(), 0);
}

// The three seconds are the clock's, not sixty sleeps of 50 ms: with its timers late, a stop that
// counted its sleeps took eight seconds and more. The macOS runners of CI failed this check with
// spent over 6000, so the stop is made here while the process is throttled.
TEST(a_child_that_ignores_sigterm_is_killed_after_three_seconds) {
    Child child("trap '' TERM; echo ready >&2; exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "ready")); // the trap is installed
    const int64_t pid = node.pid();
    CHECK(pid > 0);
    throttled(true);
    const auto began = std::chrono::steady_clock::now();
    node.stop();
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();
    throttled(false);
    CHECK(!node.running());
    CHECK(spent >= 3000); // it was given its three seconds
    CHECK(spent < 6000);  // and then the kill
    errno = 0;
    CHECK(::kill(pid_t(pid), 0) == -1 && errno == ESRCH); // gone, and not a zombie left behind
}

TEST(its_own_children_go_with_it) {
    Child child("sleep 2");
    const auto pidFile = child.config.root + "/grandchild.pid";
    child.config.bin =
        scriptFor("sh -c 'trap \"\" TERM; sleep 30' & echo $! > " + pidFile + "; wait");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(until([&] { return !fileText(pidFile).empty(); }));
    const auto grandchild = pid_t(std::stoll(fileText(pidFile)));
    CHECK(grandchild > 1);
    CHECK(::kill(grandchild, 0) == 0); // it is there, and it ignores SIGTERM
    node.stop();
    CHECK(until([&] { return ::kill(grandchild, 0) == -1; })); // the group went together
}

TEST(the_child_knows_which_process_is_its_parent) {
    Child child("sleep 2");
    const auto pidFile = child.config.root + "/parent.pid";
    child.config.bin = scriptFor("echo $YNOTBIT_PARENT_PID > " + pidFile);
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(until([&] { return !fileText(pidFile).empty(); }));
    CHECK_EQ(std::stoll(fileText(pidFile)), int64_t(::getpid())); // what the bridge checks
}

TEST(stopping_what_is_not_running_is_harmless) {
    Child child("exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    node.stop(); // never started
    node.stop();
    CHECK(!node.running());
    CHECK(!node.poll());
    CHECK(node.start());
    CHECK(node.running());
    node.stop();
    node.stop(); // and twice after a real one
    CHECK(!node.running());
}

TEST(a_child_that_cannot_be_started_is_reported) {
    Child child("exit 0");
    child.config.bin = (std::filesystem::path(child.config.root) / "no-such-node").string();
    bmc::NodeSupervisor node(child.config);
    CHECK(!node.start());
    CHECK(!node.running());
    CHECK_EQ(node.pid(), int64_t(0));
    CHECK(!node.lastError().empty());
    CHECK(!node.poll());
}
#endif // !_WIN32

namespace {
// The caller polls once a tick; the tests wait for the news the way it arrives.
bool waitFor(const std::function<bool()> &done, int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 20) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}
} // namespace

#ifdef BMC_TEST_NODE
// The node binary itself, started as the app starts its node: --node and the flags its settings
// make. On Windows this is CreateProcess with an environment block; 0.8.0 passed it without
// CREATE_UNICODE_ENVIRONMENT, Windows answered "The parameter is incorrect", the app showed that
// and no node ever started -- and nothing in CI started a node through the supervisor there.
TEST(a_node_comes_up_when_the_supervisor_starts_it) {
    bmc::NodeConfig config;
    config.bin = BMC_TEST_NODE;
    config.root = bmc::test::tempDir() + "/node";
    config.maildir = config.root + "/unused-maildir";
    std::filesystem::create_directories(config.root);
    config.listen = false;
    config.retentionMB = 64;
    config.retentionDays = 1;
    // Linked to a closed port and nothing else: no bootstrap, no DNS, no public peer.
    config.peer = "127.0.0.1:9";
    config.linkOnly = true;
    bmc::NodeSupervisor node(config);
    CHECK(node.start());
    CHECK(node.lastError().empty());
    CHECK(node.running());
    // status.json is made by a node that runs.
    const auto status = std::filesystem::path(config.root) / "status.json";
    CHECK(waitFor(
        [&] {
            node.poll();
            return std::filesystem::exists(status);
        },
        30000));
    CHECK(node.running());
    CHECK(node.standardError().empty());
    // And it stays: the node leaves by itself when it finds its parent gone (YNOTBIT_PARENT_PID),
    // which on Windows is a handle opened on the pid and waited on, once a second.
    CHECK(!waitFor(
        [&] {
            node.poll();
            return !node.running();
        },
        3500));
    node.stop();
    CHECK(!node.running());
}
#endif // BMC_TEST_NODE

#if defined(_WIN32) && defined(BMC_TEST_CHILD)
#define NOMINMAX
#include <windows.h>

namespace {
bmc::NodeConfig childConfig() {
    bmc::NodeConfig config;
    config.bin = BMC_TEST_CHILD;
    config.root = bmc::test::tempDir();
    config.maildir = config.root + "/unused-maildir";
    return config;
}
// What the child does, for the next start: it inherits the environment.
void plan(const std::string &actions) {
    SetEnvironmentVariableA("NODE_CHILD", actions.c_str());
}
std::string fileText(const std::string &path) {
    const auto bytes = bmc::test::readFile(path);
    return std::string(bytes.begin(), bytes.end());
}
bool processAlive(DWORD pid) {
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!handle)
        return false;
    const bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
}
bool saidIt(bmc::NodeSupervisor &node, const std::string &text) {
    return waitFor(
        [&] {
            node.poll();
            return node.standardError() == text;
        },
        10000);
}
} // namespace

TEST(the_supervisor_notices_a_child_that_exits) {
    plan("exit=3");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(node.lastError().empty());
    CHECK(waitFor([&] { return node.poll(); }, 10000)); // the first poll after the exit is the news
    CHECK(!node.running());
    CHECK_EQ(node.pid(), int64_t(0));
    CHECK(node.exited());
    CHECK(!node.crashed());
    CHECK_EQ(node.exitCode(), 3);
    CHECK(!node.poll()); // an exit is news once, not every tick
}

// Windows has no signal: what the system ends a program with is an NTSTATUS exit code.
TEST(the_supervisor_calls_an_ntstatus_exit_a_crash) {
    plan("crash");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(waitFor([&] { return node.poll(); }, 10000));
    CHECK(node.exited());
    CHECK(node.crashed());
}

TEST(the_supervisor_keeps_what_the_child_wrote_to_stderr) {
    plan("err=one;err=two;wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(saidIt(node, "two"));
    CHECK_EQ(node.standardError(), std::string("two"));           // the last line
    CHECK_EQ(node.standardErrorChunk(), std::string("one\ntwo")); // and the chunk it came in
    node.stop();
    CHECK(!node.running());
}

// The node logs to stdout and the app never showed a word of it: it goes to the null device, so
// the complaint is what the child "said", and a log of any length cannot block the child.
TEST(the_supervisor_drops_what_the_child_logs) {
    plan("err=cannot bind the port;out=[2026] loading saved object store;outspam=4000;"
         "err=done logging;wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(saidIt(node, "done logging"));
    CHECK_EQ(node.standardErrorChunk().find("loading"), std::string::npos);
    node.stop();
    CHECK(!node.running());
}

TEST(the_supervisor_restarts_a_child_and_the_stop_is_not_news) {
    plan("wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    const int64_t first = node.pid();
    CHECK(first > 0);
    CHECK(node.restart());
    CHECK(node.running());
    CHECK(node.pid() > 0);
    CHECK(node.pid() != first);
    CHECK(!node.poll()); // a stop this side asked for is not an exit to report
    node.stop();
    CHECK(!node.running());
}

// The new child starts with nothing said: Session::startNode() clears what it shows, and the
// supervisor has to clear what it read, or the dead child's last line is read as the new one's.
TEST(the_supervisor_forgets_what_the_last_child_said) {
    plan("err=the first child complained;wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(saidIt(node, "the first child complained"));
    plan("wait");
    CHECK(node.restart());
    CHECK(node.standardError().empty());
    CHECK(node.standardErrorChunk().empty());
    node.stop();
    CHECK(!node.running());
}

// There is no SIGTERM to give it three seconds with: a stop is a kill, and quick.
TEST(the_supervisor_stops_a_child_at_once) {
    plan("wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    const auto pid = DWORD(node.pid());
    CHECK(pid > 0);
    const auto began = std::chrono::steady_clock::now();
    node.stop();
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();
    CHECK(!node.running());
    CHECK(spent < 2500);
    CHECK(!processAlive(pid));
}

TEST(the_supervisor_takes_the_childs_own_children_with_it) {
    const auto pidFile = bmc::test::tempDir() + "/grandchild.pid";
    plan("grandchild=" + pidFile + ";wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(waitFor([&] { return !fileText(pidFile).empty(); }, 10000));
    const auto grandchild = DWORD(std::stoul(fileText(pidFile)));
    CHECK(processAlive(grandchild));
    node.stop();
    CHECK(waitFor([&] { return !processAlive(grandchild); }, 5000)); // the job went together
}

TEST(the_supervisor_tells_the_child_which_process_is_its_parent) {
    const auto pidFile = bmc::test::tempDir() + "/parent.pid";
    plan("parentpid=" + pidFile + ";wait");
    bmc::NodeSupervisor node(childConfig());
    CHECK(node.start());
    CHECK(waitFor([&] { return !fileText(pidFile).empty(); }, 10000));
    CHECK_EQ(std::stoul(fileText(pidFile)), std::stoul(std::to_string(GetCurrentProcessId())));
    node.stop();
}

TEST(the_supervisor_can_stop_what_is_not_running) {
    plan("wait");
    bmc::NodeSupervisor node(childConfig());
    node.stop(); // never started
    node.stop();
    CHECK(!node.running());
    CHECK(!node.poll());
    CHECK(node.start());
    CHECK(node.running());
    node.stop();
    node.stop(); // and twice after a real one
    CHECK(!node.running());
}

TEST(the_supervisor_reports_a_child_that_cannot_be_started) {
    plan("wait");
    auto config = childConfig();
    config.bin = (std::filesystem::path(config.root) / "no-such-node.exe").string();
    bmc::NodeSupervisor node(config);
    CHECK(!node.start());
    CHECK(!node.running());
    CHECK_EQ(node.pid(), int64_t(0));
    CHECK(!node.lastError().empty());
    CHECK(!node.poll());
}
#endif // _WIN32 && BMC_TEST_CHILD
