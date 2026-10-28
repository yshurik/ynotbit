#!/usr/bin/env python3
"""chat_responder --keys-file: an address made of two private keys, as a vanity address is.

No node is needed (--node-bin "" starts none): what is tested is which address the bot has, that
the keys are not said anywhere, and that a phrase and keys together, a keys file that cannot be
read or is no keys, and no data directory are refused at once, with the words to find the cause.

GOLDEN is the address of these two keys as tools/vanity's `bm_vanity --address-of` makes it, from the
same file: a second path to the same address, apart from the bot library.

usage: responder_keys_test.py CHAT_RESPONDER
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

responder = sys.argv[1]
tmp = tempfile.mkdtemp(prefix="responder-keys-")
SIGNING, ENCRYPTION = "11" * 32, "22" * 32
GOLDEN = "BM-87bptuw9kbPNbyuKNALxQfLjGcCpeYCobuC"


def write(name, content):
    path = os.path.join(tmp, name)
    with open(path, "w") as f:
        f.write(content)
    os.chmod(path, 0o600)
    return path


def refused(args, *words):
    """The bot is started with args and must stop by itself, saying every one of the words."""
    try:
        done = subprocess.run([responder, *args], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as e:
        raise AssertionError(f"{args} did not stop: it was not refused") from e
    said = done.stdout + done.stderr
    assert done.returncode != 0, f"{args} was not refused: {said}"
    for word in words:
        assert word in said, f"{args}: {word!r} is not in what it said: {said}"
    assert SIGNING not in said and ENCRYPTION not in said, "a key was said"


try:
    keys = write("keys.txt", f"# the keys of the bot\n{SIGNING}\n{ENCRYPTION}\n")
    data = os.path.join(tmp, "data")
    node_less = ["--node-bin", "", "--data-dir", data]

    # Its address is that of the keys, and nothing says the keys; it stops on SIGTERM, saying Bye.
    out = open(os.path.join(tmp, "stdout.txt"), "w")
    proc = subprocess.Popen([responder, "--keys-file", keys, *node_less], stdout=out,
                            stderr=subprocess.STDOUT)
    try:
        address, deadline = None, time.time() + 60
        while time.time() < deadline and not address and proc.poll() is None:
            time.sleep(0.2)
            m = re.search(r"Address:\s+(BM-\S+)", open(out.name, errors="replace").read())
            address = m and m.group(1)
        assert address == GOLDEN, f"the address is {address}, the keys make {GOLDEN}"
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=60) == 0, "exit status after SIGTERM"
    finally:
        if proc.poll() is None:
            proc.kill()
    said = open(out.name, errors="replace").read()
    assert "Bye." in said, said
    assert SIGNING not in said and ENCRYPTION not in said, "a key was said"
    # The vault on the disk is not the phrase's: the keys make its password (a second start with
    # the same keys opens it, and the address is the same).
    again = subprocess.Popen([responder, "--keys-file", keys, *node_less],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        line = ""
        while "Address:" not in line:
            line = again.stdout.readline()
            assert line, "it stopped before it said its address"
        assert GOLDEN in line, line
        again.send_signal(signal.SIGTERM)
        again.wait(timeout=60)
    finally:
        if again.poll() is None:
            again.kill()

    # Refused at once.
    phrase = write("phrase.txt", "a phrase\n")
    refused(["--keys-file", keys, "--phrase-file", phrase, *node_less], "--keys-file", "not both")
    refused(["--keys-file", keys, "--phrase", "a phrase", *node_less], "--keys-file", "not both")
    refused(["--keys-file", os.path.join(tmp, "nothing.txt"), *node_less], "keys file")
    refused(["--keys-file", write("zero.txt", "00" * 32 + "\n" + ENCRYPTION + "\n"), *node_less],
            "valid private keys")
    refused(["--keys-file", write("one.txt", SIGNING + "\n"), *node_less], "keys")
    refused(["--keys-file", keys, "--node-bin", ""], "--data-dir")
    print("PASS a bot made from two keys has their address, says no key, and refuses what is wrong")
except Exception as e:  # noqa: BLE001
    print(f"FAIL: {type(e).__name__}: {e}")
    sys.exit(1)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
