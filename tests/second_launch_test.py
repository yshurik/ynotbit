#!/usr/bin/env python3
"""A second ynotbit on a node folder in use wakes the first and exits cleanly.

With the window closed into the notification area, launching the app again is how a person asks for
it back; it must not die of "Another app instance is using this node folder".

usage: second_launch_test.py <ynotbit binary>
"""
import os
import subprocess
import sys
import tempfile
import time


def main():
    binary = os.path.abspath(sys.argv[1])
    directory = tempfile.mkdtemp(prefix="second-launch-")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    args = [binary, "--offline", "--data-dir", directory]
    first = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        # Not before the first holds the node folder: two launches at once would race for it.
        deadline = time.time() + 30
        while not os.path.exists(os.path.join(directory, "desktop.lock")):
            if first.poll() is not None or time.time() > deadline:
                raise AssertionError("the first instance did not start: " +
                                     first.stderr.read().decode())
            time.sleep(0.1)
        # It listens a moment after it holds the folder. Until it does, a second launch fails
        # on the lock, which is what this guards against; so ask again until it answers.
        last = ""
        while time.time() < deadline:
            second = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.PIPE)
            try:
                code = second.wait(timeout=10)
            except subprocess.TimeoutExpired:
                second.kill()
                raise AssertionError("the second launch went on running as an instance of its own")
            if code == 0:
                break
            last = second.stderr.read().decode()
            time.sleep(0.3)
        else:
            raise AssertionError("the second launch never exited cleanly; last said: " + last)
        if first.poll() is not None:
            raise AssertionError("the first instance ended with the second launch")
        print("PASS: a second launch woke the first and exited cleanly")
    finally:
        first.terminate()
        try:
            first.wait(timeout=15)
        except subprocess.TimeoutExpired:
            first.kill()


if __name__ == "__main__":
    main()
