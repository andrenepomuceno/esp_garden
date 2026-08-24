#!/usr/bin/env python3
"""Fails the build when a source file passes the size limit.

CLAUDE.md has said "no source file exceeds 1000 lines" for a while, and nothing
checked it. At the moment this script was written `data/devices.js` was at 1155
and `scripts/dev_server.py` at 1744, both clean past a rule the guide states as
fact. A rule on the honour system is a rule that documents an intention, not the
tree.

It prints the largest files on SUCCESS as well as on failure. A gate that only
speaks when it trips tells you a file crossed the line; it never tells you a
file is three commits away from crossing it, which is the moment the split is
still cheap.

Counting is `splitlines()`, not `count("\\n")`: a file with no trailing newline
would otherwise measure one short, quietly making the limit 1001.

Run:  python scripts/check_lines.py
      python scripts/check_lines.py --limit 800
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Firmware, web assets and tooling. Vendored libraries are excluded because
# their size is not ours to fix, and the gzipped copies are binary.
INCLUDED = [
    ("src", "*.cpp"),
    ("include", "*.h"),
    ("data", "*.js"),
    ("data", "*.html"),
    ("scripts", "*.py"),
]

# A test file is a list of independent cases, not one unit of logic: splitting
# it at a line count makes it harder to read, not easier. Vendored assets are
# somebody else's code.
EXCLUDED_NAMES = {
    "jquery.js",
    "sha256.js",
    "spark-md5.js",
}
EXCLUDED_DIRS = {"test", ".pio"}


def collect() -> list[tuple[int, Path]]:
    found: list[tuple[int, Path]] = []
    for directory, pattern in INCLUDED:
        base = ROOT / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob(pattern)):
            if path.name in EXCLUDED_NAMES:
                continue
            if any(part in EXCLUDED_DIRS for part in path.parts):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            found.append((len(text.splitlines()), path))
    return sorted(found, reverse=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=1000)
    parser.add_argument(
        "--top", type=int, default=5,
        help="how many of the largest files to report on success")
    args = parser.parse_args()

    files = collect()
    if not files:
        print("check_lines: nothing to check", file=sys.stderr)
        return 1

    over = [(n, p) for n, p in files if n > args.limit]

    for count, path in files[: args.top]:
        marker = "  OVER" if count > args.limit else ""
        print(f"  {count:>5}  {path.relative_to(ROOT).as_posix()}{marker}")

    if over:
        print(f"\ncheck_lines: {len(over)} file(s) over {args.limit} lines.")
        print("CLAUDE.md: a file past this gets modularised. Split it, or "
              "raise the limit deliberately and say why.")
        return 1

    headroom = args.limit - files[0][0]
    print(f"\ncheck_lines: {len(files)} files, largest is {files[0][0]} "
          f"lines ({headroom} under the {args.limit} limit).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
