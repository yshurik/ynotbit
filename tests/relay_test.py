"""Exercise the real, keyless relay over loopback; never contact public peers."""
import hashlib
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time


def sha(data):
    return hashlib.sha512(data).digest()


def frame(command, payload=b""):
    return b"\xe9\xbe\xb4\xd9" + command.encode().ljust(12, b"\0") + struct.pack(">I", len(payload)) + sha(payload)[:4] + payload


def receive(sock):
    def read(n):
        data=b""
        while len(data)<n:
            part=sock.recv(n-len(data))
            if not part:
                raise RuntimeError("peer disconnected")
            data+=part
        return data
    header=read(24)
    return header[4:16].rstrip(b"\0"),read(struct.unpack(">I",header[16:20])[0])


def wait_file(path):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if path.exists():
            return
        time.sleep(.05)
    raise AssertionError("relay did not persist valid object")


with tempfile.TemporaryDirectory() as temporary:
    root=pathlib.Path(temporary)
    listener=socket.socket()
    listener.bind(("127.0.0.1",0));listener.listen(1);listener.settimeout(8)
    port=listener.getsockname()[1]
    # A getpubkey object with a real, minimum-difficulty proof of work.
    expires=int(time.time())+3600
    rest=struct.pack(">QI",expires,0)+b"\x04\x01"+bytes(range(32))
    initial=sha(rest)
    length=8+len(rest)
    target=(1<<64)//(1000*(length+1000+(length+1000)*3600//65536))
    nonce=0
    while int.from_bytes(sha(sha(struct.pack(">Q",nonce)+initial))[:8],"big")>target:
        nonce+=1
    payload=struct.pack(">Q",nonce)+rest
    object_hash=sha(sha(payload))[:32]
    process=subprocess.Popen([sys.argv[1],"--node","-D",str(root),"-b","-B","-e","-L","-i","-P",f"127.0.0.1:{port}"],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        peer,_=listener.accept();peer.settimeout(8)
        command,_=receive(peer)
        assert command==b"version",command
        address=struct.pack(">Q",1)+b"\0"*10+b"\xff\xff\x7f\0\0\x01"+struct.pack(">H",port)
        agent=b"/relay-test:/"
        version=struct.pack(">IQQ",3,1,int(time.time()))+address+address+struct.pack(">Q",123456)+bytes([len(agent)])+agent+b"\x01\x01"
        peer.sendall(frame("version",version)+frame("verack"))
        peer.sendall(frame("inv",b"\x01"+object_hash))
        while True:
            command,data=receive(peer)
            if command==b"getdata":
                assert object_hash in data
                break
        peer.sendall(frame("object",payload))
        saved=root/"objects"/object_hash.hex();wait_file(saved)
        assert saved.read_bytes()==payload
        assert not list(root.rglob("keys.dat"))
        assert not list(root.rglob("maildir"))
        print("PASS: real loopback handshake, proof-of-work validation, keyless object storage")
    finally:
        process.terminate()
        try: out,err=process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill();out,err=process.communicate()
        listener.close()
        if process.returncode:
            print(out.decode(errors="replace"),err.decode(errors="replace"),file=sys.stderr)
