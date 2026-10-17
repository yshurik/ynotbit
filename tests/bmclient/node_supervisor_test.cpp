// The node process: started, noticed when it goes, asked what it said, and stopped. The children
// are shell scripts of the test's own — no node binary, no port and no network are involved.
#include <bmc/node_supervisor.h>
#include <bmc/test.h>
#ifndef _WIN32
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
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

TEST(a_child_that_ignores_sigterm_is_killed_after_three_seconds) {
    Child child("trap '' TERM; echo ready >&2; exec sleep 30");
    bmc::NodeSupervisor node(child.config);
    CHECK(node.start());
    CHECK(untilSaid(node, "ready")); // the trap is installed
    const int64_t pid = node.pid();
    CHECK(pid > 0);
    const auto began = std::chrono::steady_clock::now();
    node.stop();
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();
    CHECK(!node.running());
    CHECK(spent < 6000); // its three seconds, then the kill
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
