#!/usr/bin/env python3
"""An echo bot in Python on libbmbot (ctypes): it answers every chat with what it was told.

    python3 echo.py --phrase-file secret.txt --data-dir ./echo-data --public

It prints its address; write to it from ynotbit (Chats > New chat). The library finds bmnode beside
itself, on the PATH, or at $BMBOT_NODE, and finds itself at $BMBOT_LIB (or next to this file's
build directory). Without --public the bot links to --peer alone (default ynotbit's own node,
127.0.0.1:8444).

A real bot would limit how much it answers (see tools/chat_bot.h): two echo bots would talk
for ever.
"""
import argparse
import ctypes
import json
import os
import signal
import sys

OK, TIMEOUT = 0, 1


class BotError(Exception):
    def __init__(self, code, text):
        super().__init__(f"{text} (code {code})")
        self.code = code


def load(path=None):
    """The library, with the prototypes of bmbot.h."""
    if path is None:
        path = os.environ.get("BMBOT_LIB")
    if not path:
        here = os.path.dirname(os.path.abspath(__file__))
        for build in ("build-core", "build"):
            for name in ("libbmbot.dylib", "libbmbot.so"):
                candidate = os.path.join(here, "..", "..", build, "bmbot", name)
                if os.path.isfile(candidate):
                    path = candidate
                    break
            if path:
                break
    if not path or not os.path.isfile(path):
        raise RuntimeError("libbmbot not found: set BMBOT_LIB")
    lib = ctypes.CDLL(path)
    pp = ctypes.POINTER(ctypes.c_void_p)  # a char ** that the library fills and bmbot_free frees
    lib.bmbot_abi_version.restype = ctypes.c_int
    lib.bmbot_open.argtypes = [ctypes.c_char_p, pp, pp]
    lib.bmbot_close.argtypes = [ctypes.c_void_p]
    lib.bmbot_close.restype = None
    lib.bmbot_next.argtypes = [ctypes.c_void_p, ctypes.c_int, pp, pp]
    lib.bmbot_done.argtypes = [ctypes.c_void_p, ctypes.c_char_p, pp]
    lib.bmbot_send_chat.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, pp, pp]
    lib.bmbot_send_letter.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, pp, pp]
    lib.bmbot_state.argtypes = [ctypes.c_void_p, ctypes.c_char_p, pp, pp]
    lib.bmbot_status.argtypes = [ctypes.c_void_p, pp, pp]
    lib.bmbot_free.argtypes = [ctypes.c_void_p]
    lib.bmbot_free.restype = None
    return lib


class Bot:
    def __init__(self, config, lib=None):
        self.lib = lib or load()
        self.handle = ctypes.c_void_p()
        self._check(self.lib.bmbot_open(json.dumps(config).encode(), ctypes.byref(self.handle), *self._slot()))

    def _slot(self):
        self._error = ctypes.c_void_p()
        return (ctypes.byref(self._error),)

    def _text(self, pointer):
        if not pointer.value:
            return None
        text = ctypes.string_at(pointer.value).decode()
        self.lib.bmbot_free(pointer.value)
        return text

    def _check(self, code):
        if code < 0:
            raise BotError(code, self._text(self._error) or "error")
        return code

    def _call(self, function, *args):
        """Calls function(handle, *args, &out, &error); the string it returns, or None."""
        out = ctypes.c_void_p()
        code = function(self.handle, *args, ctypes.byref(out), *self._slot())
        self._check(code)
        return code, self._text(out)

    def next(self, timeout_ms=1000):
        """The next event as a dict, or None if none came."""
        code, text = self._call(self.lib.bmbot_next, timeout_ms)
        return None if code == TIMEOUT else json.loads(text)

    def done(self, event_id):
        self._check(self.lib.bmbot_done(self.handle, event_id.encode(), *self._slot()))

    def send_chat(self, to, text, sender=""):
        return self._call(self.lib.bmbot_send_chat, sender.encode(), to.encode(), text.encode())[1]

    def send_letter(self, to, subject, body, sender=""):
        return self._call(self.lib.bmbot_send_letter, sender.encode(), to.encode(), subject.encode(), body.encode())[1]

    def state(self, chat_id):
        return self._call(self.lib.bmbot_state, chat_id.encode())[1]

    def status(self):
        return json.loads(self._call(self.lib.bmbot_status)[1])

    def close(self):
        if self.handle:
            self.lib.bmbot_close(self.handle)
            self.handle = ctypes.c_void_p()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--phrase-file", required=True)
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--public", action="store_true", help="join the public network")
    parser.add_argument("--peer", default="127.0.0.1:8444")
    parser.add_argument("--listen", action="store_true")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    node = {"public": args.public, "listen": args.listen}
    if not args.public:
        node["peer"] = args.peer
    if args.port:
        node["port"] = args.port
    bot = Bot({"data_dir": args.data_dir, "phrase_file": args.phrase_file, "label": "Echo", "node": node, "hang_seconds": 120})
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    print("Address:", bot.status()["address"], flush=True)
    try:
        while not stop:
            event = bot.next(1000)
            if event is None:
                continue
            if event["kind"] == "note":
                print("note:", event["text"], flush=True)
                continue
            if event["kind"] == "chat":
                bot.send_chat(event["from"], "echo: " + event["text"], sender=event["to"])
                print("echoed a chat from", event["from"][:12], flush=True)
            bot.done(event["id"])  # read only now: a crash before this answers it again
    finally:
        bot.close()
    print("Bye.", flush=True)


if __name__ == "__main__":
    sys.exit(main())
