"""Exercise the real, keyless relay over loopback; never contact public peers."""
import hashlib
import os
import json
import uuid
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
    process=subprocess.Popen([sys.argv[1],os.environ.get("YNOTBIT_TEST_NODE_FLAG","--node"),"-D",str(root),"-b","-B","-e","-L","-i","-P",f"127.0.0.1:{port}"],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        peer,_=listener.accept();peer.settimeout(8)
        command,_=receive(peer)
        assert command==b"version",command
        address=struct.pack(">Q",1)+b"\0"*10+b"\xff\xff\x7f\0\0\x01"+struct.pack(">H",port)
        agent=b"/relay-test:/"
        version=struct.pack(">IQQ",3,1,int(time.time()))+address+address+struct.pack(">Q",123456)+bytes([len(agent)])+agent+b"\x01\x01"
        peer.sendall(frame("verack")+frame("version",version) if len(sys.argv)>2 else frame("version",version))
        peer.sendall(frame("inv",b"\x01"+object_hash))
        while True:
            command,data=receive(peer)
            if command==b"getdata":
                assert object_hash in data
                break
        # The node has sent getdata but we (the test) haven't answered yet --
        # deterministically exercises the "still downloading" pending count,
        # not a race against a real second peer answering instantly.
        pending_deadline=time.monotonic()+8
        pending=0
        while time.monotonic()<pending_deadline:
            pending=json.loads((root/"status.json").read_text()).get("pending",0)
            if pending>=1:
                break
            time.sleep(.05)
        assert pending>=1,"status.json should report the outstanding getdata as pending"
        peer.sendall(frame("object",payload))
        saved=root/"objects"/object_hash.hex();wait_file(saved)
        assert saved.read_bytes()==payload
        settle_deadline=time.monotonic()+8
        pending=1
        while time.monotonic()<settle_deadline:
            pending=json.loads((root/"status.json").read_text()).get("pending",1)
            if pending==0:
                break
            time.sleep(.05)
        assert pending==0,"pending should drop back to 0 once the object is received"
        # Submit a local object through the actual ciphertext bridge and fetch it as a peer.
        # Reusing a valid inventory object also exercises crash/idempotent replay handling.
        job=str(uuid.uuid4())
        spool=root/"publish"/(job+".object")
        spool.parent.mkdir(exist_ok=True)
        spool.write_bytes(payload)
        receipt=root/"receipts"/(job+".json")
        wait_file(receipt)
        result=json.loads(receipt.read_text())
        if len(sys.argv)==2:
            assert result["state"]=="accepted",result
            assert spool.exists(),"No peer yet: keep ciphertext queued"
            peer.sendall(frame("verack"))
            deadline=time.monotonic()+8
            while result["state"]!="offered" and time.monotonic()<deadline:
                time.sleep(.05)
                result=json.loads(receipt.read_text())
        assert result["state"]=="offered",result
        assert result["hash"]==object_hash.hex(),result
        peer.sendall(frame("getdata",b"\x01"+object_hash))
        while True:
            command,data=receive(peer)
            if command==b"object":
                assert data==payload
                break
        # Invalid local objects get a visible rejection, never a false success.
        rejected=str(uuid.uuid4())
        (root/"publish"/(rejected+".object")).write_bytes(b"invalid")
        rejected_receipt=root/"receipts"/(rejected+".json")
        wait_file(rejected_receipt)
        assert json.loads(rejected_receipt.read_text())["state"]=="rejected"
        assert json.loads((root/"status.json").read_text())["peers"]==1
        assert not list(root.rglob("keys.dat"))
        assert not list(root.rglob("maildir"))
        print("PASS: real loopback handshake, proof-of-work validation, keyless object storage, publication, peer fetch, rejected malformed job, pending-download status")
    finally:
        process.terminate()
        try: out,err=process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill();out,err=process.communicate()
        listener.close()
        if process.returncode:
            print(out.decode(errors="replace"),err.decode(errors="replace"),file=sys.stderr)
