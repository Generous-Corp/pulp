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
import posixpath
import re
import shlex
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


def split_compile_command(command: str, *, windows: bool | None = None) -> list[str]:
    """Split a compile database ``command`` using the producer platform's rules."""
    if windows is None:
        windows = os.name == "nt"
    if not windows:
        return shlex.split(command)

    # CommandLineToArgvW/MSVCRT quoting: backslashes are special only when
    # immediately followed by a quote.  Keep this local and dependency-free so
    # Windows command-form databases do not pass literal quote characters on to
    # the compiler.
    args: list[str] = []
    arg: list[str] = []
    in_quotes = False
    index = 0
    while index < len(command):
        char = command[index]
        if char.isspace() and not in_quotes:
            if arg:
                args.append("".join(arg))
                arg = []
            index += 1
            continue
        if char == "\\":
            run = index
            while index < len(command) and command[index] == "\\":
                index += 1
            count = index - run
            if index < len(command) and command[index] == '"':
                arg.extend("\\" * (count // 2))
                if count % 2:
                    arg.append('"')
                else:
                    in_quotes = not in_quotes
                index += 1
            else:
                arg.extend("\\" * count)
            continue
        if char == '"':
            in_quotes = not in_quotes
        else:
            arg.append(char)
        index += 1
    if arg:
        args.append("".join(arg))
    return args


def load_module_flags(cc_path: Path) -> dict[str, tuple[list[str], str]]:
    """module-root -> (filtered compile flags, working directory).

    Prefer a TU compiled by the module's canonical ``pulp-<module>`` target.
    A source file can be reused by another target in a different module; taking
    the first matching path would then test public headers with the consumer's
    incomplete include closure instead of their owning target's flags.
    """
    entries = json.loads(cc_path.read_text())
    by_module: dict[str, tuple[list[str], str]] = {}
    scores: dict[str, int] = {}
    for e in entries:
        f = e.get("file", "")
        mod = module_of(f)
        if not mod:
            continue
        args = e.get("arguments")
        if args is None:
            args = split_compile_command(e.get("command", ""))
        directory = e.get("directory", ".")
        score = module_target_score(args, mod, directory, e.get("output", ""))
        if mod in by_module and score <= scores[mod]:
            continue
        by_module[mod] = (filter_args(args, f), directory)
        scores[mod] = score
    return by_module


def module_target_score(
    args: list[str], module: str, directory: str = ".", explicit_output: str = ""
) -> int:
    """Rank a compile entry by how directly its object belongs to ``module``."""
    output = explicit_output
    for index, arg in enumerate(args[:-1]):
        if arg == "-o" or arg.lower() == "/fo":
            output = args[index + 1]
            break
    if not output:
        for arg in args:
            if arg.startswith("-o") and len(arg) > 2:
                output = arg[2:]
                break
            if arg.lower().startswith("/fo") and len(arg) > 3:
                output = arg[3:]
                break
    normalized_output = output.replace("\\", "/")
    normalized_directory = directory.replace("\\", "/").rstrip("/")
    if normalized_output.startswith("/") or re.match(r"^[A-Za-z]:/", normalized_output):
        normalized = posixpath.normpath(normalized_output)
    else:
        normalized = posixpath.normpath(f"{normalized_directory}/{normalized_output}")
    module_name = module.rsplit("/", 1)[-1].replace("_", "-")
    canonical_marker = f"CMakeFiles/pulp-{module_name}.dir/"
    if canonical_marker in normalized:
        return 2
    if f"{module}/CMakeFiles/" in normalized:
        return 1
    return 0


def filter_args(args: list[str], src_file: str) -> list[str]:
    """Strip the input file, object output, and -c; keep include/define/std."""
    out: list[str] = []
    skip_next = False
    for i, a in enumerate(args):
        lower = a.lower()
        if skip_next:
            skip_next = False
            continue
        if i == 0:
            continue  # the compiler executable; we supply our own
        if a == "-c" or lower == "/c":
            continue
        if a == "-o" or a.lower() == "/fo":
            skip_next = True
            continue
        if (
            (a.startswith("-o") and lower[2:].endswith((".o", ".obj")))
            or (lower.startswith("/fo") and lower[3:].endswith((".o", ".obj")))
            or lower.endswith((".o", ".obj"))
        ):
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
