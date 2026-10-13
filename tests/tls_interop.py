#!/usr/bin/env python3
"""The node's TLS against a peer set up the way PyBitmessage sets up its own.

PyBitmessage (src/network/tls.py) switches to TLS after the verack exchange
when the other side advertises NODE_SSL, with anonymous ECDH on secp256k1;
the side that accepted the connection is the server. It reads exactly one
header and then one payload at a time, so it never reads past the verack.

Checks, each with a fresh node on loopback:
  inbound   the peer connects to the node (node = TLS server)
  outbound  the node connects to the peer (node = TLS client)
  node      two nodes connect to each other
and that an addr shorter than its count (MiNode's) keeps the connection.

usage: tls_interop.py <ynotbit binary>
"""
import hashlib
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import warnings

# PyBitmessage's own settings use the deprecated names on purpose.
warnings.simplefilter("ignore", DeprecationWarning)

MAGIC = bytes.fromhex("e9beb4d9")
NODE_NETWORK, NODE_SSL = 1, 2


def packet(command, payload=b""):
    return (MAGIC + command.encode().ljust(12, b"\0") + struct.pack(">I", len(payload))
            + hashlib.sha512(payload).digest()[:4] + payload)


def net_address(port):
    return struct.pack(">Q", NODE_NETWORK | NODE_SSL) + bytes(10) + b"\xff\xff" + \
        socket.inet_aton("127.0.0.1") + struct.pack(">H", port)


def version(port):
    agent = b"/PyBitmessage:0.6.3.2/"
    payload = struct.pack(">IQq", 3, NODE_NETWORK | NODE_SSL, int(time.time()))
    payload += net_address(port) + net_address(port)
    payload += os.urandom(8) + bytes([len(agent)]) + agent + b"\x01\x01"
    return packet("version", payload)


def read_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise EOFError("connection closed after %d of %d bytes" % (len(data), n))
        data += chunk
    return data


def read_packet(sock):
    header = read_exact(sock, 24)
    assert header[:4] == MAGIC, "bad magic %r" % header[:4]
    length = struct.unpack(">I", header[16:20])[0]
    payload = read_exact(sock, length)
    assert hashlib.sha512(payload).digest()[:4] == header[20:24], "bad checksum"
    return header[4:16].rstrip(b"\0").decode(), payload


def context():
    # PyBitmessage's settings for OpenSSL 1.1+: protocol.sslProtocolCiphers
    # and TLSDispatcher.do_tls_init.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS)
    ctx.set_ciphers("AECDH-AES256-SHA@SECLEVEL=0")
    ctx.set_ecdh_curve("secp256k1")
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.options = (ssl.OP_ALL | ssl.OP_NO_SSLv2 | ssl.OP_NO_SSLv3 | ssl.OP_SINGLE_ECDH_USE
                   | ssl.OP_CIPHER_SERVER_PREFERENCE | ssl.OP_NO_TLSv1_3)
    return ctx


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Node:
    def __init__(self, binary, port, peer=None):
        self.dir = tempfile.mkdtemp(prefix="tls-node-")
        self.log = os.path.join(self.dir, "node.log")
        args = [binary, "--node", "-D", self.dir, "-m", os.path.join(self.dir, "maildir"),
                "-a", "127.0.0.1:%d" % port, "-e", "-b", "-B", "-L", "-l", self.log]
        if peer:
            args += ["-P", "127.0.0.1:%d" % peer]
        self.process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.port = port

    def wait_log(self, text, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(self.log) and text in open(self.log, errors="replace").read():
                return True
            time.sleep(0.1)
        return False

    def wait_listening(self):
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("node did not listen on %d" % self.port)

    def stop(self):
        self.process.terminate()
        self.process.wait(10)

    def tail(self):
        return open(self.log, errors="replace").read()[-3000:] if os.path.exists(self.log) else ""


def expect_commands(tls, wanted):
    seen = set()
    tls.settimeout(10)
    while not wanted <= seen:
        command, _ = read_packet(tls)
        seen.add(command)
    return seen


def check_inbound(binary):
    node = Node(binary, free_port())
    try:
        node.wait_listening()
        sock = socket.create_connection(("127.0.0.1", node.port), 10)
        sock.sendall(version(node.port))
        got = {read_packet(sock)[0], read_packet(sock)[0]}
        assert got == {"version", "verack"}, got
        sock.sendall(packet("verack"))
        tls = context().wrap_socket(sock, server_side=False)
        print("inbound: %s %s" % (tls.version(), tls.cipher()[0]))
        tls.sendall(packet("getaddr"))
        expect_commands(tls, {"addr", "getaddr"})
        assert node.wait_log("TLS established"), node.tail()
        # MiNode 0.3.3 sends addr whose count is one more than its entries.
        # The node keeps the entries and the connection.
        entry = struct.pack(">qIQ", int(time.time()), 1, NODE_NETWORK) + \
            bytes(10) + b"\xff\xff" + socket.inet_aton("10.1.2.3") + struct.pack(">H", 8444)
        tls.sendall(packet("addr", b"\x02" + entry) + packet("ping"))
        expect_commands(tls, {"pong"})
        print("inbound: a short addr keeps the connection")
        tls.close()
    finally:
        node.stop()


def check_outbound(binary):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(30)
    port = listener.getsockname()[1]
    node = Node(binary, free_port(), peer=port)
    try:
        sock, _ = listener.accept()
        sock.settimeout(10)
        command, _ = read_packet(sock)
        assert command == "version", command
        # PyBitmessage's order on an accepted connection: verack, version.
        sock.sendall(packet("verack") + version(port))
        command, _ = read_packet(sock)
        assert command == "verack", command
        tls = context().wrap_socket(sock, server_side=True)
        print("outbound: %s %s" % (tls.version(), tls.cipher()[0]))
        expect_commands(tls, {"addr", "getaddr"})
        assert node.wait_log("TLS established"), node.tail()
        tls.close()
    finally:
        node.stop()
        listener.close()


def check_node_to_node(binary):
    first = Node(binary, free_port())
    try:
        first.wait_listening()
        second = Node(binary, free_port(), peer=first.port)
        try:
            assert first.wait_log("TLS established"), first.tail()
            assert second.wait_log("TLS established"), second.tail()
            print("node to node: TLS established both ways")
        finally:
            second.stop()
    finally:
        first.stop()


def main():
    binary = os.path.abspath(sys.argv[1])
    try:
        context()
    except ssl.SSLError as e:
        print("SKIP: this Python's OpenSSL lacks PyBitmessage's cipher (%s)" % e)
        return 0
    check_inbound(binary)
    check_outbound(binary)
    check_node_to_node(binary)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
