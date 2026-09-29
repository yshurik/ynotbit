#!/usr/bin/env python3
"""Fail when a translation is missing, unfinished, or breaks its placeholders.

Every translations/ynotbit_*.ts must translate every message, keep the same
%1/%2 placeholders and line breaks as the English source, and the error
catalogue must match the current storage/protocol messages.
"""
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent


def placeholders(text):
    return sorted(re.findall(r"%\d", text or ""))


def main():
    problems = []
    files = sorted((ROOT / "translations").glob("ynotbit_*.ts"))
    if not files:
        problems.append("no translations/ynotbit_*.ts files")
    for path in files:
        for message in ET.parse(path).getroot().iter("message"):
            source = message.find("source").text or ""
            translation = message.find("translation")
            text = translation.text or ""
            where = f"{path.name}: {source[:60]!r}"
            if translation.get("type") in ("unfinished", "vanished", "obsolete") or not text:
                problems.append(f"{where} is not translated")
            elif placeholders(text) != placeholders(source):
                problems.append(f"{where} changes placeholders")
            elif text.count("\n") != source.count("\n"):
                problems.append(f"{where} changes line breaks")
    catalogue = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "update-error-catalog.py"), "--check"])
    if catalogue.returncode:
        problems.append("src/i18n/error_catalog.cpp is out of date")
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print(f"PASS: {len(files)} translations complete")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
