#!/usr/bin/env python3
"""safe_path_guard_lint.py — untrusted-path guards must use the shared helper.

A guard that screens an untrusted relative path (an archive entry, a manifest
member, a declared asset) with a bare ``is_absolute()`` or ``is_relative()``
check is wrong on Windows: ``/etc/passwd``, ``\\rooted`` and ``C:foo`` are all
not absolute there, and joining any of them onto a directory escapes it.
``pulp::runtime::is_safe_relative_path`` refuses every rooted form on every
platform, so each such guard must call it.

The lint finds boolean functions in ``core/`` and ``tools/cli/`` whose name
says they judge a path as safe (``safe`` together with ``path``, ``rel``,
``member``, ``archive`` or ``name``) and fails if the body tests
``is_absolute()`` or ``is_relative()`` without calling the helper.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error.

Bypass: put ``// safe-path-lint: skip <reason>`` on the function's first line.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SCOPES = ("core", "tools/cli")
SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".mm"}
GUARD_NAME = re.compile(r"\bbool\s+((?:is_)?\w*safe\w*)\s*\(", re.IGNORECASE)
PATH_WORD = re.compile(r"path|rel|member|archive|name", re.IGNORECASE)
BARE_CHECK = re.compile(r"\.\s*is_(?:absolute|relative)\s*\(\s*\)")
HELPER = "is_safe_relative_path"
HELPER_HEADER = "core/runtime/include/pulp/runtime/safe_relative_path.hpp"
SKIP = "safe-path-lint: skip"


def function_body(text: str, start: int) -> str | None:
    """Return the brace-delimited body following ``start``, or None for a declaration."""
    i = start
    depth_paren = 0
    while i < len(text):
        c = text[i]
        if c == "(":
            depth_paren += 1
        elif c == ")":
            depth_paren -= 1
        elif depth_paren == 0 and c == ";":
            return None
        elif depth_paren == 0 and c == "{":
            break
        i += 1
    else:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i : j + 1]
    return None


def scan_text(text: str, rel: str) -> list[str]:
    out: list[str] = []
    if rel.replace("\\", "/") == HELPER_HEADER:
        return out
    for match in GUARD_NAME.finditer(text):
        name = match.group(1)
        if not PATH_WORD.search(name):
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        line_end = text.find("\n", match.start())
        if SKIP in text[line_start : line_end if line_end != -1 else len(text)]:
            continue
        body = function_body(text, match.end() - 1)
        if body is None:
            continue
        if BARE_CHECK.search(body) and HELPER not in body:
            line = text.count("\n", 0, match.start()) + 1
            out.append(
                f"{rel}:{line}: {name}() screens a path with a bare "
                f"is_absolute()/is_relative(); call pulp::runtime::{HELPER}"
            )
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    root = args.root.resolve()
    violations: list[str] = []
    scanned = 0
    try:
        for scope in SCOPES:
            for path in sorted((root / scope).rglob("*")):
                if path.suffix not in SUFFIXES or not path.is_file():
                    continue
                scanned += 1
                text = path.read_text(encoding="utf-8", errors="replace")
                violations.extend(scan_text(text, str(path.relative_to(root))))
    except OSError as exc:
        print(f"safe-path-lint: {exc}", file=sys.stderr)
        return 2
    if scanned == 0:
        print(f"safe-path-lint: no sources found under {root}", file=sys.stderr)
        return 2
    if violations:
        print("safe-path-lint: untrusted-path guard without the shared helper\n")
        for v in violations:
            print(f"  {v}")
        print(
            "\nOn Windows `/x`, `\\x` and `C:x` are not absolute, so a bare "
            "is_absolute() check lets them\nreplace the destination's root. "
            "pulp::runtime::is_safe_relative_path refuses them everywhere."
        )
        return 1
    print(f"safe-path-lint: ok ({scanned} source(s) scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
