#!/usr/bin/env python3
"""bmbot from Python through ctypes, on a plain image with no Qt and no Go.

The ABI first (codes, strings, a bot with no node), then bmbot/examples/echo.py as a real bot:
a chat_responder says hello to it over two real bmnodes and its answer comes back.

usage: ctypes_test.py            with BMBOT_LIB, CHAT_RESPONDER and BMNODE in the environment
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

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "bmbot", "examples"))
import echo  # noqa: E402

lib_path = os.environ.get("BMBOT_LIB")
responder = os.environ.get("CHAT_RESPONDER")
node = os.environ.get("BMNODE")
if not lib_path or not os.path.isfile(lib_path):
    print("SKIP: set BMBOT_LIB to libbmbot", file=sys.stderr)
    sys.exit(0)
lib = echo.load(lib_path)
tmp = tempfile.mkdtemp(prefix="bmbot-ctypes-")
procs = []


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


try:
    assert lib.bmbot_abi_version() == 1

    # A bad configuration is refused with a code and a reason, not a crash.
    try:
        echo.Bot({"data_dir": os.path.join(tmp, "x")}, lib)
        raise AssertionError("a bot without a phrase opened")
    except echo.BotError as e:
        assert e.code == -1 and "phrase" in str(e), e

    # A bot with no node: it opens, says what it is, times out, and refuses a bad address.
    bot = echo.Bot({"data_dir": os.path.join(tmp, "plain"), "phrase": "ctypes test", "node": {"bin": ""}}, lib)
    status = bot.status()
    assert status["address"].startswith("BM-") and status["node"]["managed"] is False
    assert bot.next(100) is None
    try:
        bot.send_chat("not an address", "hi")
        raise AssertionError("a chat to nobody was queued")
    except echo.BotError as e:
        assert e.code == -1
    queued = bot.send_chat("BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN", "hello")
    assert queued and bot.state(queued)
    assert bot.state("no such chat") == ""
    bot.close()
    bot.close()  # harmless twice

    if responder and node:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        env = dict(os.environ, BMBOT_LIB=lib_path, BMBOT_NODE=node)
        work_a, work_b = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        os.makedirs(work_a)
        os.makedirs(work_b)
        for work, text in ((work_a, "ctypes echo bot"), (work_b, "ctypes hello bot")):
            with open(os.path.join(work, "phrase.txt"), "w") as f:
                f.write(text + "\n")
        out_a = open(os.path.join(work_a, "out.txt"), "w")
        a = subprocess.Popen([sys.executable, "-B", os.path.join(here, "..", "..", "bmbot", "examples", "echo.py"),
                              "--phrase-file", os.path.join(work_a, "phrase.txt"), "--data-dir", os.path.join(work_a, "data"),
                              "--peer", "127.0.0.1:1", "--listen", "--port", str(port)],
                             stdout=out_a, stderr=subprocess.STDOUT, env=env)
        procs.append(a)
        deadline = time.time() + 60
        address = None
        while time.time() < deadline and not address:
            m = re.search(r"Address:\s+(BM-\S+)", read(os.path.join(work_a, "out.txt")))
            address = m and m.group(1)
            time.sleep(0.2)
        assert address, "the echo bot printed no address: " + read(os.path.join(work_a, "out.txt"))
        out_b = open(os.path.join(work_b, "out.txt"), "w")
        b = subprocess.Popen([responder, "--phrase-file", os.path.join(work_b, "phrase.txt"), "--data-dir", os.path.join(work_b, "data"),
                              "--peer", f"127.0.0.1:{port}", "--silent", "--delay", "0", "--hello", address, "--node-bin", node],
                             stdout=out_b, stderr=subprocess.STDOUT)
        procs.append(b)
        deadline = time.time() + 420
        while time.time() < deadline and "echo: hello from" not in read(os.path.join(work_b, "out.txt")):
            time.sleep(0.5)
        assert "echo: hello from" in read(os.path.join(work_b, "out.txt")), \
            "no echo came back:\n" + read(os.path.join(work_a, "out.txt")) + "\n---\n" + read(os.path.join(work_b, "out.txt"))
        for p in (b, a):
            p.send_signal(signal.SIGTERM)
        for p in (b, a):
            assert p.wait(timeout=60) == 0
        assert "Bye." in read(os.path.join(work_a, "out.txt"))
    print("PASS bmbot through ctypes")
except Exception as e:  # noqa: BLE001
    print("FAIL:", type(e).__name__, e)
    sys.exit(1)
finally:
    for p in procs:
        if p.poll() is None:
            p.kill()
    shutil.rmtree(tmp, ignore_errors=True)
