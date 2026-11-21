#!/usr/bin/env python3
"""scripts/sdk_tags.py: which tags the release workflow pushes the SDK image under, and whether it pushes at all.

usage: sdk_tags_test.py            (no arguments: it finds the script next to this tests directory)
"""
import os
import subprocess
import sys
import tempfile
import unittest

here = os.path.dirname(os.path.abspath(__file__))
script = os.path.join(here, "..", "scripts", "sdk_tags.py")
IMAGE = "ghcr.io/yshurik/ynotbit-sdk"


def run(ref, tag, abi=1, header=True):
    """The script's exit code and its output as a dict of the key=value lines."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "bmbot.h")
        with open(path, "w") as f:
            f.write("#pragma once\n" + (f"#define BMBOT_ABI_VERSION {abi}\n" if header else "// no version here\n"))
        p = subprocess.run([sys.executable, "-B", script, "--ref-name", ref, "--sdk-tag", tag, "--abi-header", path],
                           capture_output=True, text=True)
    out = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
    return p.returncode, out, p.stderr


class SdkTags(unittest.TestCase):
    def test_a_version_tag_is_pushed_under_its_version_and_the_abi_tag(self):
        code, out, err = run("v0.8.4", "-")
        self.assertEqual(code, 0, err)
        self.assertEqual(out["tags"], f"{IMAGE}:0.8.4,{IMAGE}:abi1")
        self.assertEqual((out["push"], out["version"], out["abi"]), ("true", "0.8.4", "1"))

    def test_a_dispatch_with_a_tag_pushes_that_tag_only(self):
        code, out, err = run("-", "dev1")
        self.assertEqual(code, 0, err)
        self.assertEqual(out["tags"], f"{IMAGE}:dev1")
        self.assertEqual((out["push"], out["version"]), ("true", "dev1"))

    def test_a_dispatch_with_no_tag_builds_and_pushes_nothing(self):
        code, out, err = run("-", "-")
        self.assertEqual(code, 0, err)
        self.assertEqual(out["tags"], f"{IMAGE}:dev")
        self.assertEqual((out["push"], out["version"]), ("false", "dev"))

    def test_the_abi_tag_follows_the_header(self):
        code, out, err = run("v1.0.0", "-", abi=2)
        self.assertEqual(code, 0, err)
        self.assertEqual(out["tags"], f"{IMAGE}:1.0.0,{IMAGE}:abi2")
        self.assertEqual(out["abi"], "2")

    def test_a_prerelease_does_not_move_the_abi_tag(self):
        code, out, err = run("v0.9.0-rc1", "-")
        self.assertEqual(code, 0, err)
        self.assertEqual(out["tags"], f"{IMAGE}:0.9.0-rc1")
        self.assertEqual(out["push"], "true")

    def test_an_explicit_tag_wins_over_a_version_ref(self):
        code, out, err = run("v0.8.4", "try")
        self.assertEqual(code, 0, err)
        self.assertEqual(out["tags"], f"{IMAGE}:try")

    def test_a_tag_docker_would_not_take_is_refused(self):
        for bad in ("bad tag", "a;b", "-x", "x" * 200, "a/b"):
            code, out, _ = run("-", bad)
            self.assertNotEqual(code, 0, f"accepted {bad!r}")
            self.assertNotIn("tags", out)

    def test_a_header_with_no_abi_version_is_refused(self):
        code, out, _ = run("v0.8.4", "-", header=False)
        self.assertNotEqual(code, 0)
        self.assertNotIn("tags", out)


if __name__ == "__main__":
    unittest.main()
