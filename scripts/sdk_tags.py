#!/usr/bin/env python3
"""Which tags the release workflow pushes the SDK image under (docker/Dockerfile, target sdk), and whether it pushes.

usage: sdk_tags.py --ref-name NAME|- --sdk-tag TAG|- --abi-header bmbot/bmbot.h

Prints key=value lines for $GITHUB_OUTPUT: tags (a comma list), push (true or false), version (the image's
version label) and abi (BMBOT_ABI_VERSION of the header).
  - a run on a version tag vX.Y.Z: pushed as X.Y.Z and as abi<N>, the moving tag of the newest build of that ABI;
  - a run on vX.Y.Z-suffix (a prerelease): pushed as X.Y.Z-suffix only, so it never moves abi<N>;
  - a manual run with an sdk tag: pushed as that tag only (the tag wins over the ref);
  - a manual run with none: built as dev, not pushed.
A tag docker would not take, or a header without BMBOT_ABI_VERSION, is an error and prints nothing.
"""
import argparse
import re
import sys

IMAGE = "ghcr.io/yshurik/ynotbit-sdk"
TAG = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$")
RELEASE = re.compile(r"^v(\d+\.\d+\.\d+)$")
PRERELEASE = re.compile(r"^v(\d+\.\d+\.\d+-[A-Za-z0-9.-]+)$")


def abi_of(header):
    with open(header, encoding="utf-8") as f:
        found = re.search(r"(?m)^#define\s+BMBOT_ABI_VERSION\s+(\d+)\b", f.read())
    if not found:
        sys.exit(f"sdk_tags: no BMBOT_ABI_VERSION in {header}")
    return found.group(1)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ref-name", required=True, help="the git ref name of the run, or - for none")
    parser.add_argument("--sdk-tag", required=True, help="the tag typed for a manual run, or - for none")
    parser.add_argument("--abi-header", required=True, help="bmbot/bmbot.h")
    args = parser.parse_args(argv)

    abi = abi_of(args.abi_header)
    if args.sdk_tag not in ("", "-"):
        version, extra, push = args.sdk_tag, [], True
    elif RELEASE.match(args.ref_name):
        version, extra, push = RELEASE.match(args.ref_name).group(1), [f"abi{abi}"], True
    elif PRERELEASE.match(args.ref_name):
        version, extra, push = PRERELEASE.match(args.ref_name).group(1), [], True
    else:
        version, extra, push = "dev", [], False

    names = [version] + extra
    for name in names:
        if not TAG.match(name):
            sys.exit(f"sdk_tags: {name!r} is not a tag docker takes")
    print("tags=" + ",".join(f"{IMAGE}:{name}" for name in names))
    print("push=" + ("true" if push else "false"))
    print(f"version={version}")
    print(f"abi={abi}")


if __name__ == "__main__":
    main(sys.argv[1:])
