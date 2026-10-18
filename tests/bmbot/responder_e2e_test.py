#!/usr/bin/env python3
"""Two chat_responders over two real bmnodes, on this machine and nothing else.

B says hello to A (--hello); A answers it ("You said: ..."); B logs the answer. That is the whole
path: the bot library, its node as a child process, proof of work, the objects across a link, the
acknowledgment. It also checks the status file, --check, and a graceful stop (SIGTERM, "Bye.",
exit 0, the node gone).

usage: responder_e2e_test.py CHAT_RESPONDER BMNODE
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

responder, node = sys.argv[1], sys.argv[2]
tmp = tempfile.mkdtemp(prefix="bmbot-e2e-")
procs = []


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def start(name, *extra):
    work = os.path.join(tmp, name)
    os.makedirs(work)
    phrase = os.path.join(work, "phrase.txt")
    with open(phrase, "w") as f:
        f.write(f"bmbot end to end phrase of {name}\n")
    os.chmod(phrase, 0o600)
    out = open(os.path.join(work, "stdout.txt"), "w")
    cmd = [responder, "--phrase-file", phrase, "--data-dir", os.path.join(work, "data"),
           "--node-bin", node, "--delay", "0", "--log", os.path.join(work, "responder.log"),
           "--status-file", os.path.join(work, "status.json"), *extra]
    proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
    procs.append(proc)
    return proc, work


def wait_for(what, predicate, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.3)
    raise AssertionError(f"timed out waiting for {what}")


def fail(message):
    print("FAIL:", message)
    for name in ("a", "b"):
        print(f"--- {name} stdout")
        print(read(os.path.join(tmp, name, "stdout.txt")))
    sys.exit(1)


try:
    port = free_port()
    # A listens; its peer is a port nobody has. A private bot links to its peer alone.
    a, wa = start("a", "--peer", "127.0.0.1:1", "--listen", "--port", str(port))
    address_a = wait_for("A's address", lambda: (re.search(r"Address:\s+(BM-\S+)", read(os.path.join(wa, "stdout.txt"))) or [None, None])[1], 60)
    b, wb = start("b", "--peer", f"127.0.0.1:{port}", "--silent", "--hello", address_a)
    address_b = wait_for("B's address", lambda: (re.search(r"Address:\s+(BM-\S+)", read(os.path.join(wb, "stdout.txt"))) or [None, None])[1], 60)
    assert address_a != address_b

    # The answer to B's hello comes back: the getpubkey, the pubkey, the chat, the ack, the reply.
    wait_for("the answer in B's log", lambda: f"You said: hello from {address_b}" in read(os.path.join(wb, "stdout.txt")), 420)
    log_a = read(os.path.join(wa, "stdout.txt"))
    assert f"hello from {address_b}" in log_a, "A logged the chat it took up"
    assert "-> " in log_a, "A logged its answer"

    # Both are linked to each other, and say so in their status files.
    for work in (wa, wb):
        wait_for(f"a peer in {work}/status.json", lambda: re.search(r'"peers":[1-9]', read(os.path.join(work, "status.json"))), 120)
        check = subprocess.run([responder, "--check", os.path.join(work, "status.json")], capture_output=True, text=True)
        assert check.returncode == 0, check.stderr
    unhealthy = subprocess.run([responder, "--check", os.path.join(tmp, "nothing.json")], capture_output=True, text=True)
    assert unhealthy.returncode == 1
    stale = subprocess.run([responder, "--check", os.path.join(wa, "status.json"), "--peers", "99"], capture_output=True, text=True)
    assert stale.returncode == 1 and "peers" in stale.stderr

    # The status file is made again every ten seconds: the chat A took up shows in it within them.
    status_file = os.path.join(wa, "status.json")
    wait_for("A's counters in its status file",
             lambda: re.search(r'"received":[1-9]', read(status_file)) and re.search(r'"answered":[1-9]', read(status_file)), 60)

    # A graceful stop: Bye., exit 0, and the node goes with its bot.
    for proc in (b, a):
        proc.send_signal(signal.SIGTERM)
    for proc in (b, a):
        assert proc.wait(timeout=60) == 0, "exit status after SIGTERM"
    for work in (wa, wb):
        assert "Bye." in read(os.path.join(work, "stdout.txt"))
        assert "Bye." in read(os.path.join(work, "responder.log")), "the log file has it too"
    if shutil.which("ps"):  # a slim container has none
        ps = subprocess.run(["ps", "-ax", "-o", "command="], capture_output=True, text=True).stdout
        assert tmp not in ps, "a node of the test is still running:\n" + "\n".join(l for l in ps.splitlines() if tmp in l)
    print("PASS two responders chatted over real nodes, and stopped cleanly")
except Exception as e:  # noqa: BLE001
    fail(f"{type(e).__name__}: {e}")
finally:
    for proc in procs:
        if proc.poll() is None:
            proc.kill()
    shutil.rmtree(tmp, ignore_errors=True)
