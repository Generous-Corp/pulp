#!/usr/bin/env python3
"""Guard against unbounded — and, on shared hosts, whole-machine — build parallelism.

Two distinct failure classes:

* **bare** — a ``cmake --build … --parallel`` (or ``make -j`` / ``ninja`` with
  no job count) delegates to the generator's default. For the Unix Makefiles
  generator that is ``make -j`` with *no limit*: one compile per ready
  translation unit, which can exhaust memory and oversubscribe cores. This is
  wrong on *any* host, so it is flagged everywhere.

* **whole-machine** — an *explicit* job count that reads the host's core count
  directly (``-j$(sysctl -n hw.ncpu)`` / ``-j$(nproc)`` /
  ``--parallel $(getconf _NPROCESSORS_ONLN)`` / ``%NUMBER_OF_PROCESSORS%``).
  It is bounded — it has a count — yet it claims *every* core, so N concurrent
  builds request N × cores and starve each other. On a GitHub-hosted ephemeral
  runner nothing else is on the box, so this is exactly right; on a SHARED host
  (the local dev Macs, where several agents and a validation lane build at once)
  it is the melt. The distinction is a property of the **host**, not the command
  — so whole-machine is flagged only on the shared-host surfaces below, and left
  alone on the ephemeral-CI surfaces.

A build should take a *share* of a shared host, not the whole thing: a governed
path (``pulp build``, or ``tools/ci/governed-build.sh`` which acquires a tartci
lease) or a derived slice (``-j$(( $(nproc) / 4 ))``) or a literal.

This is a *static ratchet*, not a proof that every generated build command is
bounded: it scans the build-command surfaces and fails on the two shapes above.
Commands assembled at runtime (e.g. the CLI's lease-derived job count) are
bounded by their own code + unit tests, not by this scan. Deliberately
low-false-positive: comment lines are ignored, markdown is scanned inside fenced
*shell* blocks only (so docs can quote the trap in prose without self-tripping),
and the bounded forms below are recognized explicitly.

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

# Build-command surfaces where a bare flag is the only concern. These execute in
# varied contexts (some on ephemeral CI, some locally), so only the always-wrong
# `bare` class is enforced here. Directories are scanned by suffix; explicit
# files are always scanned. Kept narrow on purpose — this is about *executed
# build strings*, not prose.
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

# Surfaces whose build commands run on a SHARED host — the local dev Macs, where
# several agents and a validation lane build at once. Here (and ONLY here) a
# whole-machine job count is also a finding: on an ephemeral runner `-j$(nproc)`
# is the correct way to use the box; on a shared Mac it is the melt. The
# distinction is the host, not the command.
SHARED_HOST_FILES = [
    "CLAUDE.md",              # the command agents copy-paste onto the shared Mac
    ".shipyard/config.toml",  # validation lanes run on the local self-hosted Macs
]
SHARED_HOST_DIRS = {
    ".agents/skills": (".md",),  # read by Claude Code AND Codex, run on the dev box
}

# Markdown is scanned for *executed* commands only — the fenced shell blocks a
# reader copies. Prose about `--parallel` (or a `-j$(nproc)` quoted as the trap
# to avoid) is documentation, not a build command, and the guard has no business
# failing on it.
FENCE = re.compile(r'^\s*```')
SHELL_FENCE = re.compile(r'^\s*```(bash|sh|shell|console|zsh)?\s*$', re.IGNORECASE)

# Only lines that actually invoke a build tool are considered — this keeps the
# scan off `date -j`, string-literal flag names (`arg == "--parallel"`), and
# other non-build uses of the same tokens.
BUILD_CONTEXT = re.compile(r'cmake\s+--build|(?<![\w-])make\b|\bninja\b|msbuild|xcodebuild',
                           re.IGNORECASE)

# A `--parallel` / `-j` occurrence is BOUNDED when the value that follows is a
# count, a shell expansion, a Windows env var, or (in C++ source) a concatenated
# job expression:
#   --parallel=8   --parallel 8   -j 8   -j8
#   --parallel $(getconf …)   -j$(nproc)   -j"$JOBS"   -j"${JOBS}"   `nproc`
#   --parallel %NUMBER_OF_PROCESSORS%                 (Windows cmd)
#   " … --parallel " + std::to_string(jobs)          (C++ concat)
# The value region is: optional `=`/whitespace, optional quote, then a digit,
# `$`, backtick, or `%` — OR the C++ close-quote-then-`+` form. (Whether such a
# count is *whole-machine* is a separate axis, classified below.)
BOUNDED = re.compile(r'''([=\s]*["']?[\d$`%])|(\s*"\s*\+)''')

PARALLEL = re.compile(r'--parallel')
DASH_J = re.compile(r'(?<![\w-])-j(?![\w])')  # `-j` as its own flag, not `-june`

# A job count can be explicit and still claim the WHOLE machine. These
# expansions read the host's core count directly; the job count a build should
# use comes from the governor (`pulp build`, or a tartci host-profile lease),
# never from the hardware.
CORE_COUNT_EXPANSION = re.compile(
    r'''sysctl\s+-n\s+hw\.(ncpu|logicalcpu|physicalcpu)'''
    r'''|(?<![\w-])nproc(?![\w])'''
    r'''|getconf\s+_NPROCESSORS_ONLN'''
    r'''|%NUMBER_OF_PROCESSORS%'''
    r'''|hardware_concurrency''',
    re.IGNORECASE)

# The core count is fine as an *input* to a share — `$(($(nproc) / 4))`, or a
# `min(...)` clamp. Only an undivided read is whole-machine. The division must be
# arithmetic: a `$(( … ))`, or a `/`/`%` fenced by spaces. A bare `[/%]` would
# read the slash in a `2>/dev/null` redirection as a divisor and wave the
# whole-machine command through.
SHARE_ARITHMETIC = re.compile(r'\$\(\(|\s/\s|\s%\s|\bmin\b', re.IGNORECASE)

# Shell noise that carries path separators but no arithmetic. Stripped before the
# share check so a redirection cannot masquerade as a division.
REDIRECTION = re.compile(r'\d?>&?\s*\S*|<\s*\S*')

# How far past the flag to look for the value's expansion. A job value is short;
# this keeps the scan off a core-count read elsewhere on a long command line.
VALUE_WINDOW = 48

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


def _classify(code: str, end: int) -> str | None:
    """Classify the job value that follows a `--parallel`/`-j` at `end`.

    Returns "bare" (no count at all), "whole-machine" (an undivided read of the
    host's core count), or None when the value is a bounded share."""
    rest = code[end:]
    if not BOUNDED.match(rest):
        return "bare"
    value = REDIRECTION.sub(" ", rest[:VALUE_WINDOW])
    if CORE_COUNT_EXPANSION.search(value) and not SHARE_ARITHMETIC.search(value):
        return "whole-machine"
    return None


def _shell_fenced_lines(text: str) -> set[int]:
    """Line numbers inside a fenced shell block — the copy-pasteable commands in a
    markdown file. Everything else in a .md is prose.

    Open/close is tracked by *any* fence; whether the open block is shell is
    decided once, when it opens, from the opening fence's language tag. A closing
    fence is a bare ```` ``` ````, which `SHELL_FENCE` also matches — so it must
    NOT be re-read as opening a shell block, or the prose after a ```` ```python ````
    block would be scanned as commands."""
    inside = False
    is_shell = False
    live: set[int] = set()
    for lineno, raw in enumerate(text.splitlines(), start=1):
        if FENCE.match(raw):
            if inside:
                inside = False        # this fence closes the current block
                is_shell = False
            else:
                inside = True         # this fence opens a block
                is_shell = SHELL_FENCE.match(raw) is not None
            continue
        if inside and is_shell:
            live.add(lineno)
    return live


def scan_file_kinds(path: Path, shared_host: bool | None = None) -> list[tuple[int, str, str]]:
    """Return [(lineno, snippet, kind)] for every finding, where kind is
    "bare" or "whole-machine". Whole-machine findings are suppressed unless the
    surface is a shared host."""
    findings: list[tuple[int, str, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return findings
    suffix = path.suffix
    if shared_host is None:
        shared_host = is_shared_host_surface(path)
    fenced = _shell_fenced_lines(text) if suffix == ".md" else None
    for lineno, line in _logical_lines(text):
        if fenced is not None and lineno not in fenced:
            continue
        code = _comment_cut(line, suffix)
        if not code or not BUILD_CONTEXT.search(code):
            continue
        for pattern in (PARALLEL, DASH_J):
            for m in pattern.finditer(code):
                kind = _classify(code, m.end())
                # A whole-machine count is only a finding on a shared host; on an
                # ephemeral runner it is the correct way to use the box.
                if kind == "whole-machine" and not shared_host:
                    continue
                if kind:
                    findings.append((lineno, line.strip(), kind))
    return findings


def scan_file(path: Path) -> list[tuple[int, str]]:
    """Back-compat shape: [(lineno, snippet)] for every finding on `path`."""
    return [(lineno, snippet) for lineno, snippet, _kind in scan_file_kinds(path)]


def is_shared_host_surface(path: Path) -> bool:
    try:
        rel = path.resolve().relative_to(REPO_ROOT)
    except ValueError:
        return False
    if str(rel) in SHARED_HOST_FILES:
        return True
    return any(str(rel).startswith(d + os.sep) for d in SHARED_HOST_DIRS)


def iter_default_targets() -> list[Path]:
    targets: list[Path] = []
    for rel in SCAN_FILES + SHARED_HOST_FILES:
        p = REPO_ROOT / rel
        if p.is_file() and p not in targets:
            targets.append(p)
    for rel, suffixes in {**SCAN_DIRS, **SHARED_HOST_DIRS}.items():
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

    bare: list[str] = []
    whole: list[str] = []
    for path in targets:
        for lineno, snippet, kind in scan_file_kinds(path):
            try:
                rel = path.relative_to(REPO_ROOT)
            except ValueError:
                rel = path
            (bare if kind == "bare" else whole).append(f"{rel}:{lineno}: {snippet}")

    if bare:
        print("build_parallelism_guard: FAIL — bare --parallel/-j (no job count):",
              file=sys.stderr)
        for f in bare:
            print(f"  {f}", file=sys.stderr)
        print("\nAdd an explicit job count (a literal, a $(…) expansion, or a "
              "concatenated job expression). A bare --parallel maps to unbounded "
              "make -j and can melt a shared machine.", file=sys.stderr)

    if whole:
        print("build_parallelism_guard: FAIL — whole-machine job count on a shared "
              "host (explicit, but claims every core):", file=sys.stderr)
        for f in whole:
            print(f"  {f}", file=sys.stderr)
        print("\nA core-count expansion (`$(nproc)`, `$(sysctl -n hw.ncpu)`, "
              "`getconf _NPROCESSORS_ONLN`) is a count, so it is not *unbounded* — "
              "but on a shared host it asks for the whole machine, so N concurrent "
              "builds request N x cores and starve each other (and the required "
              "macos gate validating in one of those checkouts starves with them). "
              "Prefer `pulp build` / `tools/ci/governed-build.sh`, which take their "
              "job count from the host governor. In a raw command, derive a share "
              "($(( $(nproc) / 4 ))) or use a literal. This is a shared-host rule: "
              "on a GitHub-hosted ephemeral runner `-j$(nproc)` is correct and is "
              "NOT flagged.", file=sys.stderr)

    if bare or whole:
        return 1

    print(f"build_parallelism_guard: OK — {len(targets)} build surface(s) bounded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
