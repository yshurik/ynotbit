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


LITERALS = r'("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)'
MARKED = re.compile(r'(?:\btr\(|QT_TRANSLATE_NOOP3?\("[^"]*",)\s*' + LITERALS, re.S)


def source_strings():
    """Every string the UI sources mark for translation."""
    found = set()
    for name in ("desktop_window.cpp", "session.cpp"):
        text = (ROOT / "src" / name).read_text(encoding="utf-8")
        for match in MARKED.finditer(text):
            parts = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))
            found.add("".join(parts).encode("utf-8").decode("unicode_escape")
                      .encode("latin-1").decode("utf-8"))
    return found


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
    # A string added to the code but never extracted would ship in English.
    if files:
        extracted = {m.find("source").text for m in ET.parse(files[0]).getroot().iter("message")}
        for text in sorted(source_strings() - extracted):
            problems.append(f"{text[:60]!r} is not in the .ts files; run the update_translations target")
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
