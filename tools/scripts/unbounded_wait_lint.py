#!/usr/bin/env python3
"""Fail on a newly introduced UNBOUNDED wait in a test.

A concurrency test routinely has to wait for a worker to reach some state before
it can assert anything. Three shapes are used for that in this repo, and only
one of them can fail:

    while (!started.load(...)) std::this_thread::yield();   // hangs
    started_future.wait();                                  // hangs
    cv.wait(lock, [&]{ return started; });                  // hangs

If a genuine regression means the worker never reaches that state, none of the
above ever return. The suite does not fail — it parks, and CI reports a job
timeout with no output, which is strictly worse than the flake the wait was
added to fix. A wait must be bounded so the awaited outcome that never arrives
becomes a named, failed assertion:

    CHECK(pulp::test::wait_for_condition(
        [&] { return started.load(std::memory_order_acquire); }));
    CHECK(fut.wait_for(pulp::test::kProgressDeadline) == std::future_status::ready);
    CHECK(cv.wait_for(lock, 10s, [&]{ return started; }));

`test/support/thread_progress.hpp` provides the helpers.

This is deliberately DIFF-SCOPED: it flags only added/changed lines, so the
pre-existing backlog of unbounded waits never blocks a PR while the population
stops growing. Draining that backlog needs a reproduction per site and is
separate work.

It is a best-effort lexical scan. A wait whose bound lives elsewhere (a worker
loop whose flag is always set by a joined thread, a wait already inside a
deadline loop) can be suppressed with a trailing `// unbounded-wait: allow
<reason>` comment. Give a reason — "the OS eventually schedules a runnable
thread" is NOT one, because it does not cover the case where the code under test
stops handing the waiter a value.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional

SUPPRESSION = "unbounded-wait: allow"

# Directories whose waits this guard governs. Production code has its own
# blocking-call rules; this is about tests parking a CI job.
SCOPED_PREFIXES = ("test/",)
SOURCE_SUFFIXES = (".cpp", ".hpp", ".mm", ".h", ".cc")

# A clock or deadline mentioned near the wait means the author bounded it.
BOUNDED_HINT = re.compile(
    r"steady_clock|system_clock|deadline|wait_for|wait_until|timeout|kProgressDeadline"
)

# `while (!flag.load(...))` / `while (flag.load(...) == 0)` — a spin on a state
# some OTHER thread must publish. A `stop`/`running`-style flag is the worker's
# own exit condition, not a wait, so it is not flagged.
SPIN = re.compile(r"\bwhile\s*\(\s*!?\s*[A-Za-z_][\w.\->]*\.load\s*\(")
WORKER_EXIT_FLAG = re.compile(r"\b(stop|stop_requested|running|keep_going|quit)\b")

# `cv.wait(lock, pred)` — the two-argument predicate form has no timeout.
CV_WAIT = re.compile(r"\b\w*(cv|condition|changed|state_changed)\w*\.wait\s*\(\s*\w+\s*,")

# A bare `.wait();` on a future/promise handle. `cp.wait()` / `child.wait()` are
# process waits (a different animal with its own timeout plumbing).
FUTURE_WAIT = re.compile(r"\b([A-Za-z_]\w*)\s*\.\s*wait\s*\(\s*\)\s*;")
PROCESS_WAIT_RECEIVERS = {"cp", "child", "proc", "process", "moved", "assigned"}


class Finding:
    def __init__(self, rel: str, line_no: int, kind: str, text: str) -> None:
        self.rel = rel
        self.line_no = line_no
        self.kind = kind
        self.text = text

    def __str__(self) -> str:
        return f"{self.rel}:{self.line_no}: {self.kind}\n      {self.text.strip()}"


def _run_git(root: Path, args: list[str]) -> str:
    try:
        out = subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True, check=False
        )
    except OSError:
        return ""
    return out.stdout if out.returncode == 0 else ""


def _is_git_repo(root: Path) -> bool:
    return bool(_run_git(root, ["rev-parse", "--git-dir"]).strip())


def _added_lines(root: Path, diff_args: list[str]) -> dict[str, set[int]]:
    """Map path -> set of added line numbers from a unified-zero diff."""
    text = _run_git(root, ["diff", "--unified=0", "--no-color", *diff_args])
    result: dict[str, set[int]] = {}
    current: Optional[str] = None
    for line in text.splitlines():
        if line.startswith("+++ b/"):
            current = line[6:]
        elif line.startswith("@@") and current:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if m:
                start = int(m.group(1))
                count = int(m.group(2) or "1")
                result.setdefault(current, set()).update(range(start, start + count))
    return result


def _untracked_files(root: Path) -> list[str]:
    text = _run_git(root, ["ls-files", "--others", "--exclude-standard"])
    return [p for p in text.splitlines() if p]


def changed_line_map(root: Path, base: str, head: str) -> Optional[dict[str, set[int]]]:
    """Added lines across the branch diff plus staged/unstaged/untracked work.

    Matches docs_noise_lint: a local run before committing sees the same surface
    pre-push will enforce after committing.
    """
    if not _is_git_repo(root):
        return None
    combined: dict[str, set[int]] = {}
    for args in ([f"{base}...{head}"], ["--cached"], []):
        for path, lines in _added_lines(root, args).items():
            combined.setdefault(path, set()).update(lines)
    for path in _untracked_files(root):
        full = root / path
        if full.is_file():
            try:
                n = len(full.read_text(errors="replace").splitlines())
            except OSError:
                continue
            combined.setdefault(path, set()).update(range(1, n + 1))
    return combined


def _in_scope(rel: str) -> bool:
    return rel.startswith(SCOPED_PREFIXES) and rel.endswith(SOURCE_SUFFIXES)


def _logical_statement(lines: list[str], idx: int, limit: int = 6) -> str:
    """The wait's OWN statement: `lines[idx]` plus its continuation lines.

    Scoping the bound check to the statement — rather than to a window of
    neighbours — matters. A window lets an ADJACENT bounded wait vouch for an
    unbounded one, which is the most likely arrangement in a file someone is
    part-way through fixing, and would make the new violation invisible.
    """
    out: list[str] = []
    depth = 0
    for i in range(idx, min(len(lines), idx + limit)):
        line = lines[i]
        out.append(line)
        depth += line.count("(") - line.count(")")
        if i > idx or depth <= 0:
            if depth <= 0:
                break
    return "\n".join(out)


def _classify(line: str, statement: str, preceding: str = "") -> Optional[str]:
    """Return the violation kind, or None.

    `statement` is the wait's own statement (see _logical_statement);
    `preceding` is the single line above, so a suppression comment may sit there.
    """
    if SUPPRESSION in statement or SUPPRESSION in preceding:
        return None
    if BOUNDED_HINT.search(statement):
        return None

    if SPIN.search(line) and not WORKER_EXIT_FLAG.search(line):
        return "unbounded spin on a worker-set flag (no deadline in the condition)"
    if CV_WAIT.search(line):
        return "unbounded cv.wait(lock, pred) — use wait_for(lock, timeout, pred)"
    m = FUTURE_WAIT.search(line)
    if m and m.group(1) not in PROCESS_WAIT_RECEIVERS:
        return "unbounded future.wait() — use wait_for(kProgressDeadline)"
    return None


def scan_file(root: Path, rel: str, only_lines: Optional[set[int]]) -> list[Finding]:
    # Scope is enforced here rather than only in the callers: a check that holds
    # on one path into a function and not another is the shape this whole guard
    # exists to catch.
    if not _in_scope(rel):
        return []
    path = root / rel
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return []
    findings: list[Finding] = []
    for idx, line in enumerate(lines, start=1):
        if only_lines is not None and idx not in only_lines:
            continue
        # The bound may sit on a continuation line of a multi-line condition, so
        # the statement (not the raw line) is what gets checked.
        statement = _logical_statement(lines, idx - 1)
        preceding = lines[idx - 2] if idx >= 2 else ""
        kind = _classify(line, statement, preceding)
        if kind:
            findings.append(Finding(rel, idx, kind, line))
    return findings


def scan(
    root: Path,
    explicit: Iterable[str],
    *,
    base: str,
    head: str,
    scan_all: bool,
) -> list[Finding]:
    explicit = list(explicit)
    if explicit:
        return [f for rel in explicit if _in_scope(rel) for f in scan_file(root, rel, None)]

    if scan_all:
        out: list[Finding] = []
        for path in sorted((root / "test").rglob("*")):
            rel = path.relative_to(root).as_posix()
            if path.is_file() and _in_scope(rel):
                out.extend(scan_file(root, rel, None))
        return out

    line_map = changed_line_map(root, base, head)
    if line_map is None:
        return []
    out = []
    for rel, lines in sorted(line_map.items()):
        if _in_scope(rel):
            out.extend(scan_file(root, rel, lines))
    return out


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--base", default="origin/main", help="base ref for the changed-line scan")
    parser.add_argument("--head", default="HEAD", help="head ref for the changed-line scan")
    parser.add_argument(
        "--all",
        action="store_true",
        help="scan every test source instead of only changed lines (reports the backlog)",
    )
    parser.add_argument("paths", nargs="*", help="explicit files to scan in full")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    if not root.is_dir():
        sys.stderr.write(f"[unbounded-wait-lint] error: not a directory: {root}\n")
        return 2

    findings = scan(root, args.paths, base=args.base, head=args.head, scan_all=args.all)
    if not findings:
        print("unbounded-wait-lint: ok")
        return 0

    sys.stderr.write(
        f"[unbounded-wait-lint] {len(findings)} unbounded wait(s) on changed lines:\n\n"
    )
    for f in findings:
        sys.stderr.write(f"  {f}\n\n")
    sys.stderr.write(
        "A wait that cannot time out turns a real regression into a CI job timeout with\n"
        "no output. Bound it so the outcome that never arrives fails a named assertion:\n\n"
        "  #include \"support/thread_progress.hpp\"\n"
        "  CHECK(pulp::test::wait_for_condition([&] { return started.load(); }));\n"
        "  CHECK(fut.wait_for(pulp::test::kProgressDeadline) == std::future_status::ready);\n"
        "  CHECK(cv.wait_for(lock, std::chrono::seconds(10), [&] { return ready; }));\n\n"
        f"If the wait is genuinely bounded elsewhere, add `// {SUPPRESSION} <reason>`\n"
        "with a reason that covers the code-under-test never publishing the value.\n"
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
