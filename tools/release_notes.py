#!/usr/bin/env python3
"""Extract the changelog section for a given version as release notes.

Usage: release_notes.py CHANGELOG.md VERSION

Prints the section body (without the ``## [VERSION]`` heading, without the
``---`` separator and trailing blank lines) to stdout.  Exits 1 if the
section is missing or empty, so CI can fail loudly on inconsistent release
metadata — same philosophy as the 1.x ``scripts/release_meta.py``.
"""
from __future__ import annotations

import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: release_notes.py CHANGELOG.md VERSION", file=sys.stderr)
        return 2
    path, version = sys.argv[1], sys.argv[2]

    try:
        text = open(path, encoding="utf-8").read()
    except OSError as exc:
        print(f"cannot read {path}: {exc}", file=sys.stderr)
        return 2

    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.startswith(f"## [{version}]"):
            start = i
            break
    if start is None:
        print(f"CHANGELOG has no '## [{version}]' section", file=sys.stderr)
        return 1

    end = len(lines)
    for j in range(start + 1, len(lines)):
        if lines[j].startswith("## ["):
            end = j
            break

    section = lines[start + 1 : end]
    while section and not section[-1].strip():
        section.pop()
    if section and section[-1].strip() == "---":
        section.pop()
        while section and not section[-1].strip():
            section.pop()

    body = "\n".join(section).strip("\n")
    if not body.strip():
        print(f"changelog section for {version} is empty", file=sys.stderr)
        return 1
    print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
