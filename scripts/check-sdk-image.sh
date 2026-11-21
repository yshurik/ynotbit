#!/usr/bin/env bash
# Checks an SDK image (docker/Dockerfile, target sdk): the files a bot in another repository builds on, that
# nothing the library or the node links to is missing, that the library reports the ABI of bmbot/bmbot.h,
# that bmbot.pc points at it, and that the image's labels say which ABI and which version it holds.
# Usage: scripts/check-sdk-image.sh [ssh-host] IMAGE      (with a host, docker runs there, as in build-image.sh)
# The last line is `sdk image ok`; any problem prints a line that starts with `sdk image:` and exits 1.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$#" -ge 2 ]; then host="$1"; image="$2"; else host=""; image="${1:?usage: scripts/check-sdk-image.sh [ssh-host] IMAGE}"; fi
dk() { if [ -n "$host" ]; then ssh "$host" docker "$@"; else docker "$@"; fi; }
fail() { echo "sdk image: $*" >&2; exit 1; }

abi="$(sed -n 's/^#define BMBOT_ABI_VERSION \([0-9][0-9]*\).*/\1/p' bmbot/bmbot.h)"
[ -n "$abi" ] || fail "no BMBOT_ABI_VERSION in bmbot/bmbot.h"

# Everything that needs the image's own files runs inside it; the script travels on stdin.
out="$(dk run --rm -i --entrypoint sh "$image" -s <<'INSIDE'
set -e
for f in /usr/local/include/bmbot.h /usr/local/lib/libbmbot.so.1 /usr/local/lib/pkgconfig/bmbot.pc \
         /usr/local/bin/bmnode /usr/local/share/bmbot/examples/echo.py; do
    [ -e "$f" ] || { echo "problem: missing $f"; exit 0; }
done
for b in /usr/local/lib/libbmbot.so /usr/local/bin/bmnode; do
    if ldd "$b" | grep -q 'not found'; then echo "problem: $b links to a library that is not found: $(ldd "$b" | grep 'not found' | tr -s ' \t' ' ')"; exit 0; fi
done
python3 - <<'PY'
import ctypes, os, re
lib = ctypes.CDLL("/usr/local/lib/libbmbot.so")
print("abi", lib.bmbot_abi_version())
pc = open("/usr/local/lib/pkgconfig/bmbot.pc").read()
if not re.search(r"(?m)^Version:\s*\S", pc):
    print("problem: bmbot.pc has no Version line")
values = dict(re.findall(r"(?m)^(\w+)=(.*)$", pc))
def expand(text):
    for _ in range(5):
        text = re.sub(r"\$\{(\w+)\}", lambda m: values.get(m.group(1), ""), text)
    return text
libdir = expand(values.get("libdir", ""))
if not os.path.exists(os.path.join(libdir, "libbmbot.so")):
    print("problem: bmbot.pc says libdir=%s, which holds no libbmbot.so" % libdir)
PY
INSIDE
)" || fail "the image could not be run: $image"

problems="$(printf '%s\n' "$out" | sed -n 's/^problem: //p')"
[ -z "$problems" ] || fail "$problems"
reported="$(printf '%s\n' "$out" | sed -n 's/^abi \([0-9]*\)$/\1/p')"
[ "$reported" = "$abi" ] || fail "the library reports ABI '${reported}', bmbot/bmbot.h says $abi"

# The labels are read from the full JSON (a --format template would lose its quotes on the way through ssh).
labels="$(dk image inspect "$image" | python3 -I -c '
import json, sys
labels = json.load(sys.stdin)[0]["Config"].get("Labels") or {}
print(labels.get("io.ynotbit.bmbot.abi", ""))
print(labels.get("org.opencontainers.image.version", ""))')"
label="$(printf '%s\n' "$labels" | sed -n 1p)"
version="$(printf '%s\n' "$labels" | sed -n 2p)"
[ "$label" = "$abi" ] || fail "the label io.ynotbit.bmbot.abi is '${label}', bmbot/bmbot.h says $abi"
[ -n "$version" ] || fail "the label org.opencontainers.image.version is empty"
echo "sdk image ok"
