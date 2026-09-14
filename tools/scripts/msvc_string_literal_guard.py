#!/usr/bin/env python3
"""Reject a C++ string literal larger than MSVC will accept.

MSVC rejects a single string literal over 16380 bytes with C2026 ("string too
big, trailing characters truncated"). Clang and GCC accept it, and so does
MSVC's x64 host compiler in practice -- but the ARM64 cross-compiler enforces
the cap, so an over-long literal builds everywhere a developer looks and breaks
exactly one release leg, hours later, in a job that blocks publication.

Adjacent literal concatenation does NOT help: the limit applies to the single
literal the concatenation produces. The fix is separate named literals joined
at run time (see kJSPreamblePart1/Part2 in core/view/src/widget_bridge.cpp).

This scans raw string literals (R"delim( ... )delim"), which is where the large
embedded payloads in this repo live.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# MSVC's documented cap. A literal exactly at the cap is accepted.
MSVC_LITERAL_CAP_BYTES = 16380

SCAN_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".mm"}
SCAN_ROOTS = ("core", "inspect", "ship", "tools/cli", "examples", "apple")

# R"delim( ... )delim" with an optional delimiter of up to 16 chars.
RAW_STRING = re.compile(r'R"([^()\\ ]{0,16})\(', re.DOTALL)

# Known over-cap literals that cannot break a build today, listed rather than
# excluded by narrowing the scan: anything NEW still fails closed. The threejs
# demo embeds a whole HTML/JS page and is an example, and examples are compiled
# only on Linux and macOS (clang/gcc accept the literal); nothing builds them
# with the MSVC ARM64 cross-compiler that enforces the cap.
ALLOWED = {"examples/threejs-native-demo/main.cpp"}


def oversized_literals(text: str) -> list[tuple[int, int]]:
    """(byte_length, offset) for each raw literal above the cap."""
    found = []
    for m in RAW_STRING.finditer(text):
        closing = ')' + m.group(1) + '"'
        end = text.find(closing, m.end())
        if end == -1:
            continue
        size = len(text[m.end():end].encode("utf-8"))
        if size > MSVC_LITERAL_CAP_BYTES:
            found.append((size, m.start()))
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-root", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[2])
    ap.add_argument("paths", nargs="*", type=pathlib.Path,
                    help="explicit files to scan instead of the default roots")
    args = ap.parse_args()

    if args.paths:
        files = [p for p in args.paths if p.suffix in SCAN_SUFFIXES]
    else:
        files = [p for root in SCAN_ROOTS
                 for p in (args.repo_root / root).rglob("*")
                 if p.suffix in SCAN_SUFFIXES and p.is_file()]

    if not files:
        print("msvc-string-literal-guard: no C++ sources found — refusing to "
              "report a clean scan of nothing", file=sys.stderr)
        return 2

    failures = 0
    for path in sorted(files):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        rel = path.relative_to(args.repo_root) if path.is_absolute() else path
        if rel.as_posix() in ALLOWED:
            continue
        for size, offset in oversized_literals(text):
            line = text.count("\n", 0, offset) + 1
            print(f"{rel}:{line}: raw string literal is {size} bytes, over "
                  f"MSVC's {MSVC_LITERAL_CAP_BYTES}-byte cap (C2026). Split it "
                  f"into separate named literals joined at run time.")
            failures += 1

    if failures:
        print(f"msvc-string-literal-guard: {failures} oversized literal(s)")
        return 1
    print(f"msvc-string-literal-guard: OK — {len(files)} file(s) scanned, "
          f"no literal over {MSVC_LITERAL_CAP_BYTES} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
