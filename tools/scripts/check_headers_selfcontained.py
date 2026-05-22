#!/usr/bin/env python3
"""Verify Pulp public headers are self-contained.

Each public header must compile on its own (a TU that does nothing but
``#include`` it). A header that names ``uint32_t`` without
``#include <cstdint>`` — or uses ``std::string`` without ``<string>`` — slips
past Apple Clang's lenient libc++ (Pulp's only *required* CI gate) but breaks
the Linux/Windows release build. That class caused the v0.197.4 release failure
(pulp #2576) and the #594 incidents before it.

This script catches it cheaply and deterministically: for each header it reuses
the real compile flags from ``compile_commands.json`` (matched by module) and
runs ``<compiler> -fsyntax-only`` on a one-line ``#include`` TU. It only fails
on a header that genuinely does not compile alone — so, unlike full IWYU, it has
no "unused include" false positives and is safe to gate on.

Usage:
    check_headers_selfcontained.py --compile-commands build/compile_commands.json \
        [--headers FILE | --changed BASE_REF] [--compiler clang++]

Exit code 0 = all checked headers compile standalone; 1 = one or more failed;
2 = usage / setup error.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Headers under these path fragments are skipped (generated / vendored / not
# intended to be included standalone).
SKIP_FRAGMENTS = ("/external/", "/build", "/_deps/", "_gen.hpp", ".gen.hpp")

# Match a module root like "core/<module>" so a header's flags can be borrowed
# from a translation unit compiled in the same module.
MODULE_RE = re.compile(r"(^|/)(core/[a-z0-9_]+)/")


def module_of(path: str) -> str | None:
    m = MODULE_RE.search(path.replace(os.sep, "/"))
    return m.group(2) if m else None


def load_module_flags(cc_path: Path) -> dict[str, tuple[list[str], str]]:
    """module-root -> (filtered compile flags, working directory).

    The first TU seen per module wins; its -I/-D/-std/-isystem flags apply to
    every header in that module's include tree.
    """
    entries = json.loads(cc_path.read_text())
    by_module: dict[str, tuple[list[str], str]] = {}
    for e in entries:
        f = e.get("file", "")
        mod = module_of(f)
        if not mod or mod in by_module:
            continue
        args = e.get("arguments")
        if args is None:
            # `command` form — split naively (compile_commands rarely quotes).
            args = e.get("command", "").split()
        by_module[mod] = (filter_args(args, f), e.get("directory", "."))
    return by_module


def filter_args(args: list[str], src_file: str) -> list[str]:
    """Strip the input file, object output, and -c; keep include/define/std."""
    out: list[str] = []
    skip_next = False
    for i, a in enumerate(args):
        if skip_next:
            skip_next = False
            continue
        if i == 0:
            continue  # the compiler executable; we supply our own
        if a in ("-c",):
            continue
        if a == "-o":
            skip_next = True
            continue
        if a.endswith(".o"):
            continue
        if os.path.basename(a) == os.path.basename(src_file) or a == src_file:
            continue
        out.append(a)
    return out


def check_header(header: Path, flags: list[str], cwd: str, compiler: str) -> str | None:
    """Return an error string if the header fails to compile standalone, else None."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".cpp", delete=False, dir=cwd
    ) as tu:
        tu.write(f'#include "{header.resolve()}"\n')
        tu_path = tu.name
    try:
        proc = subprocess.run(
            [compiler, *flags, "-fsyntax-only", tu_path],
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if proc.returncode != 0:
            return proc.stderr.strip() or proc.stdout.strip() or "(no diagnostics)"
        return None
    finally:
        os.unlink(tu_path)


def collect_headers(args, repo_root: Path) -> list[Path]:
    if args.headers:
        names = [l.strip() for l in Path(args.headers).read_text().splitlines() if l.strip()]
    elif args.changed:
        diff = subprocess.run(
            ["git", "diff", "--name-only", "--diff-filter=d", f"{args.changed}...HEAD"],
            cwd=repo_root, capture_output=True, text=True,
        ).stdout
        names = diff.splitlines()
    else:
        names = [str(p.relative_to(repo_root)) for p in repo_root.glob("core/*/include/pulp/**/*.hpp")]

    headers = []
    for n in names:
        if not n.endswith((".hpp", ".h", ".hh", ".hxx")):
            continue
        if any(frag in ("/" + n) for frag in SKIP_FRAGMENTS):
            continue
        if "/include/pulp/" not in n.replace(os.sep, "/"):
            continue  # only public headers
        p = (repo_root / n)
        if p.exists():
            headers.append(p)
    return headers


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compile-commands", required=True, type=Path)
    ap.add_argument("--headers", help="file listing headers to check (one per line)")
    ap.add_argument("--changed", help="base ref; check only headers changed vs BASE...HEAD")
    ap.add_argument("--compiler", default="clang++")
    ap.add_argument("--repo-root", default=".", type=Path)
    args = ap.parse_args()

    if not args.compile_commands.exists():
        print(f"error: {args.compile_commands} not found — configure with "
              f"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON first", file=sys.stderr)
        return 2

    repo_root = args.repo_root.resolve()
    by_module = load_module_flags(args.compile_commands)
    if not by_module:
        print("error: no module flags parsed from compile_commands.json", file=sys.stderr)
        return 2

    headers = collect_headers(args, repo_root)
    if not headers:
        print("No public headers to check.")
        return 0

    failures: list[tuple[Path, str]] = []
    skipped = 0
    for h in headers:
        mod = module_of(str(h))
        if not mod or mod not in by_module:
            skipped += 1
            continue
        flags, cwd = by_module[mod]
        err = check_header(h, flags, cwd, args.compiler)
        if err:
            failures.append((h, err))

    print(f"Checked {len(headers) - skipped} self-contained header(s); "
          f"{skipped} skipped (no module flags).")
    if failures:
        print(f"\n❌ {len(failures)} header(s) are NOT self-contained:\n")
        for h, err in failures:
            rel = h.relative_to(repo_root) if h.is_relative_to(repo_root) else h
            print(f"::error file={rel}::not self-contained — add the missing #include")
            print(f"--- {rel} ---\n{err}\n")
        return 1
    print("✅ all checked headers are self-contained.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
