#!/usr/bin/env python3
"""A taken listen port does not stop the node.

The node listens on a free port instead, says so in status.json, and moves
back to its own port once that frees, keeping the connections it has.

usage: listen_fallback_test.py <ynotbit binary>
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait(predicate, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError("timed out waiting for " + what)


def main():
    binary = os.path.abspath(sys.argv[1])
    directory = tempfile.mkdtemp(prefix="listen-fallback-")
    log = os.path.join(directory, "node.log")
    port = free_port()

    # Another program holds the port, on every address as the node asks.
    holder = socket.socket(socket.AF_INET6)
    holder.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    holder.bind(("::", port))
    holder.listen(1)

    env = dict(os.environ, YNOTBIT_LISTEN_RETRY_SECONDS="2")
    node = subprocess.Popen([binary, "--node", "-D", directory, "-m",
                             os.path.join(directory, "maildir"), "-p", str(port),
                             "-e", "-b", "-B", "-l", log],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def status():
        try:
            with open(os.path.join(directory, "status.json")) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def read_log():
        return open(log, errors="replace").read() if os.path.exists(log) else ""

    try:
        fallback = wait(lambda: (status() or {}).get("listen_port"), 15, "a listen port")
        assert node.poll() is None, node.stderr.read()
        assert fallback != port, "the node claims the taken port %d" % port
        assert "Cannot listen on port %d" % port in read_log(), read_log()
        kept = socket.create_connection(("127.0.0.1", fallback), 5)
        print("port %d taken: node listens on %d" % (port, fallback))

        holder.close()
        wait(lambda: (status() or {}).get("listen_port") == port, 15, "the port back")
        assert "Listening on port %d again" % port in read_log(), read_log()
        socket.create_connection(("127.0.0.1", port), 5).close()
        try:
            socket.create_connection(("127.0.0.1", fallback), 5).close()
            raise AssertionError("the node still listens on the fallback port")
        except (ConnectionRefusedError, TimeoutError):
            pass  # Windows retries a refused connect until the timeout
        # The connection made on the fallback port is still open.
        kept.setblocking(False)
        try:
            assert kept.recv(1) != b"", "the node closed a connection on the old port"
        except BlockingIOError:
            pass
        kept.close()
        print("port %d freed: node is back on it" % port)
    finally:
        node.terminate()
        node.wait(10)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
