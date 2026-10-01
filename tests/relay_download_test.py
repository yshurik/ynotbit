"""Downloads from a PyBitmessage peer, over loopback: never contact public peers.

PyBitmessage silently drops getdata that arrives in the first seconds after the
handshake, and a node that asked one peer for everything it advertised (and
then waited minutes before asking anyone else) appeared stuck on a fresh
start. The relay waits out that window, and asks each peer for a bounded
share at a time.
"""
import hashlib
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

QUIET = 10  # NTB_NETWORK_REQUEST_QUIET_TIME
PER_PEER = 500  # NTB_NETWORK_MAX_REQUESTS_PER_PEER


def sha(data):
    return hashlib.sha512(data).digest()


def frame(command, payload=b""):
    return (b"\xe9\xbe\xb4\xd9" + command.encode().ljust(12, b"\0") +
            struct.pack(">I", len(payload)) + sha(payload)[:4] + payload)


def receive(sock):
    def read(n):
        data = b""
        while len(data) < n:
            part = sock.recv(n - len(data))
            if not part:
                raise RuntimeError("peer disconnected")
            data += part
        return data
    header = read(24)
    return header[4:16].rstrip(b"\0"), read(struct.unpack(">I", header[16:20])[0])


def var_int(data):
    if data[0] < 0xfd:
        return data[0], 1
    size = {0xfd: 2, 0xfe: 4, 0xff: 8}[data[0]]
    return int.from_bytes(data[1:1 + size], "big"), 1 + size


with tempfile.TemporaryDirectory() as temporary:
    root = pathlib.Path(temporary)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]
    process = subprocess.Popen(
        [sys.argv[1], os.environ.get("YNOTBIT_TEST_NODE_FLAG", "--node"), "-D", str(root),
         "-b", "-B", "-e", "-L", "-i", "-P", f"127.0.0.1:{port}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        peer, _ = listener.accept()
        peer.settimeout(1)
        command, _ = receive(peer)
        assert command == b"version", command
        address = struct.pack(">Q", 1) + b"\0" * 10 + b"\xff\xff\x7f\0\0\x01" + struct.pack(">H", port)
        agent = b"/PyBitmessage:0.6.3.2/"
        version = (struct.pack(">IQQ", 3, 1, int(time.time())) + address + address +
                   struct.pack(">Q", 654321) + bytes([len(agent)]) + agent + b"\x01\x01")
        peer.sendall(frame("verack") + frame("version", version))
        started = time.monotonic()
        hashes = [sha(b"advertised %d" % i)[:32] for i in range(PER_PEER + 100)]
        peer.sendall(frame("inv", b"\xfd" + struct.pack(">H", len(hashes)) + b"".join(hashes)))
        requested, first = [], None
        while time.monotonic() - started < QUIET + 6:
            try:
                command, data = receive(peer)
            except socket.timeout:
                continue
            if command == b"getdata":
                if first is None:
                    first = time.monotonic() - started
                count, offset = var_int(data)
                requested += [data[offset + 32 * i:offset + 32 * (i + 1)] for i in range(count)]
        assert first is not None, "the advertised objects are requested"
        assert first >= QUIET - 1, f"no getdata inside PyBitmessage's quiet window (came at {first:.1f}s)"
        assert len(requested) == PER_PEER, f"one peer gets a bounded share at a time ({len(requested)})"
        assert len(set(requested)) == PER_PEER and set(requested) <= set(hashes), \
            "each requested hash is one it advertised, asked once"
        print(f"PASS: first getdata after {first:.1f}s, {len(requested)} of {len(hashes)} requested")
    finally:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()
