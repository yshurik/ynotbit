// The node process: started from a NodeConfig, in a process group of its own, stopped the way the
// app stops it (SIGTERM, then SIGKILL three seconds later, both at the group) and polled for what
// it says. The app keeps the timers that decide *when* a node starts or restarts; this owns how.
//
// This is the one place in the core that calls the operating system: posix_spawn, waitpid, kill
// and a pipe on POSIX; CreateProcess and a job object on Windows. Poll, do not wait: a caller
// polls once a tick, so a child that dies is reaped within a tick.
#pragma once
#include <bmc/node_config.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bmc {
class NodeSupervisor {
  public:
    explicit NodeSupervisor(NodeConfig config);
    NodeSupervisor(const NodeSupervisor &) = delete;
    NodeSupervisor &operator=(const NodeSupervisor &) = delete;
    ~NodeSupervisor(); // stop(), and never throws

    // The child runs config.bin with config.nodeArguments() after it, in the inherited environment
    // plus YNOTBIT_PARENT_PID, which is what the relay bridge checks to stop when the app is gone.
    // False, with lastError() set, when it could not be started; nothing when it is already
    // running.
    bool start();
    // Reads what the child wrote to stderr since the last call, and reaps it when it has exited.
    // True the first time it sees an exit since the last start(): the caller's cue to say why and
    // to schedule a restart.
    bool poll();
    // SIGTERM to the process group, up to three seconds, then SIGKILL: what Session did with
    // terminate/waitForFinished/kill. Harmless when nothing runs, and safe twice.
    void stop();
    // stop() then start(), as Session::restartNode did.
    bool restart();

    bool running() const;
    int64_t pid() const; // 0 when it is not running
    int exitCode() const;
    // The last exit was the operating system ending it rather than the program returning: a signal
    // (POSIX) or an NTSTATUS exit code (Windows). A stop() this class was asked for is not reported
    // by poll(), so nobody reads this for a stop; a SIGKILL of a node that ignored SIGTERM does
    // show here, as WIFSIGNALED did.
    bool crashed() const;
    bool exited() const; // an exit has been seen since the last start()
    // Why the spawn failed; "" while there is nothing to say. An exit's reason is the child's last
    // stderr line, which Session shows.
    const std::string &lastError() const;
    // The last line the child wrote to stderr, and the chunk it came in: the app shows the chunk
    // as "Node: %1" and keeps the line as the reason the node stopped. Trimmed; 200 and 400 bytes
    // at most. Both stay as they were until the child writes something new.
    const std::string &standardError() const;
    const std::string &standardErrorChunk() const;
    const NodeConfig &config() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bmc
