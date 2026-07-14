#!/usr/bin/env python3
"""Guard against unbounded build parallelism in tracked build commands.

A bare ``cmake --build … --parallel`` (or ``make -j`` / ``ninja`` with no job
count) delegates to the generator's default. For the Unix Makefiles generator
that is ``make -j`` with *no limit*: one compile per ready translation unit,
which can exhaust memory and oversubscribe cores on a shared machine. Every
build command Pulp ships must therefore pass an explicit, bounded job count
(a literal, a ``$(…)``/``${…}`` shell expansion, or — in C++ — a concatenated
job expression).

This is a *bare-flag ratchet*, not a proof that every generated build command
is bounded: it statically scans the build-command surfaces (Shipyard validation
config, the CLI/MCP command builders, shell build scripts, CI workflows) and
fails on a ``--parallel`` / ``-j`` with no following count. Commands assembled
at runtime (e.g. the CLI's lease-derived job count) are bounded by their own
code + unit tests, not by this scan. Deliberately low-false-positive: comment
lines are ignored and the bounded forms below are recognized explicitly.

Usage:
    build_parallelism_guard.py                 # scan the default surfaces
    build_parallelism_guard.py <files...>      # scan specific files
    build_parallelism_guard.py --list          # list the scanned surfaces
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Build-command surfaces. Directories are scanned by suffix; explicit files are
# always scanned. Kept narrow on purpose — this is about *executed build
# strings*, not prose.
SCAN_FILES = [
    ".shipyard/config.toml",
    "setup.sh",
]
SCAN_DIRS = {
    "tools/cli": (".cpp", ".hpp"),
    "tools/mcp": (".cpp", ".hpp"),
    "tools/scripts": (".sh",),
    "tools/validation": (".sh",),
    "tools/import-validation": (".sh",),
    "ci": (".sh", ".yml", ".yaml"),
    ".github/workflows": (".yml", ".yaml"),
}

# Only lines that actually invoke a build tool are considered — this keeps the
# scan off `date -j`, string-literal flag names (`arg == "--parallel"`), and
# other non-build uses of the same tokens.
BUILD_CONTEXT = re.compile(r'cmake\s+--build|(?<![\w-])make\b|\bninja\b|msbuild|xcodebuild',
                           re.IGNORECASE)

# A `--parallel` / `-j` occurrence is BOUNDED when the value that follows is a
# count, a shell expansion, or (in C++ source) a concatenated job expression:
#   --parallel=8   --parallel 8   -j 8   -j8
#   --parallel $(getconf …)   -j$(nproc)   -j"$JOBS"   -j"${JOBS}"   `nproc`
#   " … --parallel " + std::to_string(jobs)          (C++ concat)
# The value region is: optional `=`/whitespace, optional quote, then a digit,
# `$`, or backtick — OR the C++ close-quote-then-`+` form.
BOUNDED = re.compile(r'''([=\s]*["']?[\d$`])|(\s*"\s*\+)''')

PARALLEL = re.compile(r'--parallel')
DASH_J = re.compile(r'(?<![\w-])-j(?![\w])')  # `-j` as its own flag, not `-june`

COMMENT_PREFIXES = ("#", "//", "*", "///")


def _comment_cut(line: str, suffix: str) -> str:
    """Return the code portion of the line, dropping trailing comments so a
    `--parallel` mentioned in prose does not trip the guard."""
    stripped = line.lstrip()
    if stripped.startswith(COMMENT_PREFIXES):
        return ""
    if suffix in (".cpp", ".hpp"):
        # Drop a trailing // comment (best-effort; not string-aware, but the
        # bounded/bare check below tolerates that).
        idx = line.find("//")
        if idx != -1:
            return line[:idx]
    if suffix in (".sh", ".toml", ".yml", ".yaml", ""):
        # A `#` outside quotes starts a comment. Best-effort: only cut when the
        # `#` is preceded by whitespace (avoids chopping `$#` etc.).
        m = re.search(r'\s#', line)
        if m:
            return line[: m.start()]
    return line


def _logical_lines(text: str):
    """Yield (start_lineno, joined_line) folding shell/YAML `\\`-continuations
    into one logical line, so a build command split across physical lines
    (`cmake --build … \\` / `--parallel`) is scanned as a whole."""
    raw_lines = text.splitlines()
    i = 0
    n = len(raw_lines)
    while i < n:
        start = i + 1
        buf = raw_lines[i]
        while buf.rstrip().endswith("\\") and i + 1 < n:
            buf = buf.rstrip()[:-1] + " " + raw_lines[i + 1]
            i += 1
        yield start, buf
        i += 1


def scan_file(path: Path) -> list[tuple[int, str]]:
    findings: list[tuple[int, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return findings
    suffix = path.suffix
    for lineno, line in _logical_lines(text):
        code = _comment_cut(line, suffix)
        if not code or not BUILD_CONTEXT.search(code):
            continue
        for m in PARALLEL.finditer(code):
            if not BOUNDED.match(code[m.end():]):
                findings.append((lineno, line.strip()))
        for m in DASH_J.finditer(code):
            if not BOUNDED.match(code[m.end():]):
                findings.append((lineno, line.strip()))
    return findings


def iter_default_targets() -> list[Path]:
    targets: list[Path] = []
    for rel in SCAN_FILES:
        p = REPO_ROOT / rel
        if p.is_file():
            targets.append(p)
    for rel, suffixes in SCAN_DIRS.items():
        base = REPO_ROOT / rel
        if not base.is_dir():
            continue
        for dirpath, _dirs, files in os.walk(base):
            for name in files:
                if name.endswith(suffixes):
                    targets.append(Path(dirpath) / name)
    return targets


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--list" in args:
        for p in iter_default_targets():
            print(p.relative_to(REPO_ROOT))
        return 0

    targets = [Path(a) for a in args] if args else iter_default_targets()

    failures: list[str] = []
    for path in targets:
        for lineno, snippet in scan_file(path):
            try:
                rel = path.relative_to(REPO_ROOT)
            except ValueError:
                rel = path
            failures.append(f"{rel}:{lineno}: unbounded parallelism: {snippet}")

    if failures:
        print("build_parallelism_guard: FAIL — bare --parallel/-j (no job count):",
              file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print("\nAdd an explicit job count (a literal, a $(…) expansion, or a "
              "concatenated job expression). A bare --parallel maps to unbounded "
              "make -j and can melt a shared machine.", file=sys.stderr)
        return 1

    print(f"build_parallelism_guard: OK — {len(targets)} build surface(s) bounded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
