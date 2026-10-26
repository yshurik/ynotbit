#!/usr/bin/env python3
"""nickbot over a real bmnode, with a second bot (Python, on libbmbot through ctypes) as its client.

The client registers a nickname by chat, looks it up three ways, searches for it, is refused a
second time, writes a letter and gets a letter back, and nickbot's status file, health check, graceful
stop and registry (which admin reads) are checked at the end. Everything crosses a real link:
getpubkey, pubkey, proof of work, the objects, the acknowledgments.

usage: nickbot_e2e_test.py NICKBOT BMNODE        with BMBOT_LIB in the environment
"""
import json
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

nickbot, node = sys.argv[1], sys.argv[2]
lib_path = os.environ.get("BMBOT_LIB")
if not lib_path or not os.path.isfile(lib_path):
    print("SKIP: set BMBOT_LIB to libbmbot", file=sys.stderr)
    sys.exit(0)
lib = echo.load(lib_path)
tmp = tempfile.mkdtemp(prefix="nickbot-e2e-")
procs = []
client = None


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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
    print("--- nickbot stdout")
    print(read(os.path.join(tmp, "nickbot.out")))
    sys.exit(1)


def trailer(text):
    last = text.rstrip("\n").split("\n")[-1]
    assert last.startswith("nickbot: "), f"no trailer in {text!r}"
    value = json.loads(last[len("nickbot: "):])
    assert value["v"] == 1, value
    return value


def ask(text, *, seconds=420, kind="chat"):
    """Sends a command from the client and returns the answer's text."""
    if kind == "chat":
        client.send_chat(nickbot_address, text)
    else:
        client.send_letter(nickbot_address, text, text)
    deadline = time.time() + seconds
    while time.time() < deadline:
        event = client.next(1000)
        if event is None or event["kind"] == "note":
            continue
        client.done(event["id"])
        if event["from"] == nickbot_address and event["kind"] == kind:
            return event
    raise AssertionError(f"no answer to {text!r} within {seconds} s")


try:
    port = free_port()
    work = os.path.join(tmp, "nickbot")
    os.makedirs(work)
    phrase = os.path.join(work, "phrase.txt")
    with open(phrase, "w") as f:
        f.write("nickbot end to end phrase\n")
    os.chmod(phrase, 0o600)
    out = open(os.path.join(tmp, "nickbot.out"), "w")
    env = dict(os.environ, BMBOT_NODE=node)
    status_file = os.path.join(work, "status.json")
    proc = subprocess.Popen(
        [nickbot, "--data-dir", os.path.join(work, "data"), "--phrase-file", phrase, "--status-file", status_file,
         "--public=false", "--peer", "127.0.0.1:1", "--listen", "--port", str(port), "--retention-mb", "64",
         "--retention-days", "1"], stdout=out, stderr=subprocess.STDOUT, env=env)
    procs.append(proc)
    nickbot_address = wait_for("nickbot's address", lambda: (re.search(r"Address: (BM-\S+)", read(os.path.join(tmp, "nickbot.out"))) or [None, None])[1], 90)
    anonymous = wait_for("the lookup chan", lambda: (re.search(r"chan \"anonymous\", (BM-\S+)", read(os.path.join(tmp, "nickbot.out"))) or [None, None])[1], 5)
    assert anonymous == echo.chan_address(lib, "anonymous"), "nickbot and the library disagree about the lookup chan"

    cphrase = os.path.join(tmp, "client-phrase.txt")
    with open(cphrase, "w") as f:
        f.write("nickbot end to end client phrase\n")
    os.chmod(cphrase, 0o600)
    client = echo.Bot({"data_dir": os.path.join(tmp, "client"), "phrase_file": cphrase, "label": "client",
                       "hang_seconds": 300, "node": {"bin": node, "public": False, "peer": f"127.0.0.1:{port}"}}, lib)
    client_address = client.status()["address"]
    assert client_address != nickbot_address

    # The first answer pays for the key exchange: getpubkey, pubkey, the chat, the acknowledgment.
    event = ask("/register @e2etest")
    t = trailer(event["text"])
    assert t["cmd"] == "register" and t["ok"] is True and t["nick"] == "@e2etest" and t["address"] == client_address, t
    assert event["text"].startswith("Registered @e2etest for " + client_address), event["text"]

    t = trailer(ask("/address @E2ETEST")["text"])
    assert t["found"] is True and t["address"] == client_address and t["nick"] == "@e2etest", t

    t = trailer(ask("/nickname " + client_address)["text"])
    assert t["results"] == [{"address": client_address, "nicks": ["@e2etest"]}], t

    t = trailer(ask("/search e2et")["text"])
    assert t["results"] == [{"nick": "@e2etest", "address": client_address}] and t["more"] is False, t

    t = trailer(ask("/register @e2etest")["text"])
    assert t["ok"] is True and t["already"] is True, t

    t = trailer(ask("/register @other_name")["text"])
    assert t["ok"] is True and t["replaced"] == "@e2etest", t
    t = trailer(ask("/whoami")["text"])
    assert t["nicks"] == ["@other_name"], t

    event = ask("hello there")
    t = trailer(event["text"])
    assert t["ok"] is False and t["error"] == "unknown_command", t

    # A letter gets a letter, with no trailer.
    letter = ask("/address @other_name", kind="letter")
    assert letter["subject"].startswith("Re: "), letter
    assert "nickbot:" not in letter["text"] and letter["text"].startswith("@other_name is " + client_address), letter

    # It says what it did, and keeps saying it.
    # Eight chats and a letter were answered; the file is made again every ten seconds.
    st = json.loads(wait_for("a status file with all the counters", lambda: (lambda s: s if '"answered":' in s and json.loads(s)["answered"] >= 9 else None)(read(status_file)), 60))
    assert st["address"] == nickbot_address and st["names"] == 1 and st["letters"] == 1 and st["chats"] == 8, st
    check = subprocess.run([nickbot, "--check", status_file, "--peers", "1"], capture_output=True, text=True)
    assert check.returncode == 0, check.stderr

    # A graceful stop: Bye., exit 0, and the node goes with it.
    proc.send_signal(signal.SIGTERM)
    assert proc.wait(timeout=60) == 0, "exit status after SIGTERM"
    assert "Bye." in read(os.path.join(tmp, "nickbot.out"))
    registry = subprocess.run([nickbot, "admin", "-db", os.path.join(work, "data", "nickbot.sqlite"), "export"], capture_output=True, text=True)
    assert registry.returncode == 0 and registry.stdout.strip() == f"@other_name\t{client_address}\tno", registry
    backups = os.listdir(os.path.join(work, "data", "backup"))
    assert len(backups) == 1 and backups[0].startswith("nickbot-"), backups
    print("OK")
except Exception as e:  # an assertion, or anything else that broke
    fail(str(e))
finally:
    if client is not None:
        client.close()
    for p in procs:
        if p.poll() is None:
            p.kill()
    shutil.rmtree(tmp, ignore_errors=True)
