"""Exercise the real, keyless relay over loopback; never contact public peers."""
import hashlib
import os
import json
import re
import uuid
import pathlib
import socket
import sqlite3
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


def read_json(path):
    """A file the relay writes by replacing it. Windows refuses to open it at the moment of the
    replacement; the next try reads the new one."""
    for _ in range(200):
        try:
            return json.loads(path.read_text())
        except PermissionError:
            time.sleep(.01)
    return json.loads(path.read_text())


def wait_file(path):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if path.exists():
            return
        time.sleep(.05)
    raise AssertionError("relay did not persist valid object")


def wait_object(root,object_hash):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        db=root/"objects.sqlite"
        if db.exists():
            connection=sqlite3.connect(db)
            try:
                row=connection.execute("SELECT payload FROM objects WHERE hash=?",(object_hash,)).fetchone()
            except sqlite3.OperationalError:
                row=None
            finally:
                connection.close()
            if row:
                return row[0]
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
    process=subprocess.Popen([sys.argv[1],os.environ.get("YNOTBIT_TEST_NODE_FLAG","--node"),"-D",str(root),"-b","-B","-e","-L","-i","-R","64","-A","1","-P",f"127.0.0.1:{port}"],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        peer,_=listener.accept();peer.settimeout(8)
        command,data=receive(peer)
        assert command==b"version",command
        # 80 fixed bytes, then the user agent as a var_str (short: 1-byte length)
        agent=data[81:81+data[80]]
        assert re.fullmatch(rb"/ynotbit:\d+\.\d+\.\d+/",agent),("identifies as ynotbit and its release version",agent)
        address=struct.pack(">Q",1)+b"\0"*10+b"\xff\xff\x7f\0\0\x01"+struct.pack(">H",port)
        agent=b"/relay-test:/"
        version=struct.pack(">IQQ",3,1,int(time.time()))+address+address+struct.pack(">Q",123456)+bytes([len(agent)])+agent+b"\x01\x01"
        peer.sendall(frame("verack")+frame("version",version) if len(sys.argv)>2 else frame("version",version))
        peer.sendall(frame("inv",b"\x01"+object_hash))
        saw_getaddr=False
        while True:
            command,data=receive(peer)
            if command==b"getaddr":
                saw_getaddr=True
            if command==b"getdata":
                assert object_hash in data
                break
        if len(sys.argv)>2 and os.environ.get("YNOTBIT_TEST_NODE_FLAG","--node")=="--node":
            # Only checked in the verack-first ordering (handshake, and so
            # connection_established() which sends getaddr, is guaranteed
            # complete before this point -- in the other ordering our own
            # verack isn't sent until much later in this script, so notbit
            # may still be pipelining inv/getdata ahead of that) against the
            # real notbit engine specifically: the Qt-native relay's address
            # sharing is a separate, more minimal implementation that doesn't
            # send real addr lists either way, so it's out of scope here.
            assert saw_getaddr,("ynotbit should actively request the peer's known addresses "
                                "once connected, not only wait for unsolicited addr gossip")
        # The node has sent getdata but we (the test) haven't answered yet --
        # deterministically exercises the "still downloading" pending count,
        # not a race against a real second peer answering instantly.
        pending_deadline=time.monotonic()+8
        pending=0
        while time.monotonic()<pending_deadline:
            pending=read_json(root/"status.json").get("pending",0)
            if pending>=1:
                break
            time.sleep(.05)
        assert pending>=1,"status.json should report the outstanding getdata as pending"
        peer.sendall(frame("object",payload))
        assert wait_object(root,object_hash)==payload
        status={}
        stats_deadline=time.monotonic()+8
        while time.monotonic()<stats_deadline:
            status=read_json(root/"status.json")
            if status.get("objects",0)>=1:
                break
            time.sleep(.05)
        assert status.get("objects",0)>=1 and status.get("object_bytes",0)>=len(payload),\
            "status.json reports the stored objects and their size"
        settle_deadline=time.monotonic()+8
        pending=1
        while time.monotonic()<settle_deadline:
            pending=read_json(root/"status.json").get("pending",1)
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
        result=read_json(receipt)
        if len(sys.argv)==2:
            assert result["state"]=="accepted",result
            assert spool.exists(),"No peer yet: keep ciphertext queued"
            peer.sendall(frame("verack"))
            deadline=time.monotonic()+8
            while result["state"]!="offered" and time.monotonic()<deadline:
                time.sleep(.05)
                result=read_json(receipt)
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
        assert read_json(rejected_receipt)["state"]=="rejected"
        # Two spool entries the bridge refuses to look at: a name that is not a job id, and a
        # symlink, even one pointing at a real object. Both carry a valid object, so the only
        # thing that can refuse them is the rule being tested.
        before=read_json(root/"status.json")["time"]
        bad_name=root/"publish"/("A"*36+".object")  # A job id's length, none of its alphabet.
        bad_name.write_bytes(payload)
        target=root/"symlink-target.object"  # A real object, out of the spool's reach.
        target.write_bytes(payload)
        link=root/"publish"/(str(uuid.uuid4())+".object")
        os.symlink(target,link)
        tick_deadline=time.monotonic()+8
        while read_json(root/"status.json")["time"]<before+2:
            assert time.monotonic()<tick_deadline,"the relay stopped ticking"
            time.sleep(.05)
        assert not (root/"receipts"/(bad_name.stem+".json")).exists(),\
            "a spool name that is not a job id is not processed"
        assert not (root/"receipts"/(link.stem+".json")).exists(),\
            "a symlinked spool entry is not followed, whatever it points at"
        assert bad_name.exists() and link.is_symlink(),"refused entries are left where they are"
        assert read_json(root/"status.json")["peers"]==1
        assert not list(root.rglob("keys.dat"))
        assert not list(root.rglob("maildir"))
        print("PASS: real loopback handshake, proof-of-work validation, keyless object storage, publication, peer fetch, rejected malformed job, refused spool names and symlinks, pending-download status, active getaddr")
    finally:
        process.terminate()
        try: out,err=process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill();out,err=process.communicate()
        listener.close()
        if process.returncode:
            print(out.decode(errors="replace"),err.decode(errors="replace"),file=sys.stderr)
