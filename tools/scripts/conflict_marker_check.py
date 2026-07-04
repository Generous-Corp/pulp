#!/usr/bin/env python3
"""Guard: no git conflict markers survive in tracked files.

A merge or rebase that hits a conflict writes marker lines into the working
tree — a seven-character run of ``<``, ``>``, or ``|`` at the start of a line,
plus a bare seven-``=`` separator. Committing a file with those markers still
present yields code/config that does not compile or parse, and — when it lands
via a squash-merge that the tooling did not reject — it reaches ``main``
silently. That is exactly what happened when a stale ``chore: bump versions``
side of a squash collided with an already-advanced ``project() VERSION`` line
and the merge wrote markers straight into ``CMakeLists.txt``.

This guard makes "no committed conflict markers" an enforced, whole-tree
invariant. It keys on the start/base/end markers
(``<<<<<<<`` / ``|||||||`` / ``>>>>>>>`` followed by whitespace or end-of-line):
those line starts do not appear anywhere in the current tracked tree
(``external/`` included, verified), so in practice they are a clean signal. They
are not *guaranteed* impossible in legitimate content, though — a VCS tutorial or
a vendored test fixture could carry one — so a real exception is a deliberate,
reviewable ``ALLOWLIST`` edit. The ``=======`` separator IS common in real
content (Markdown setext headings, ASCII banners), so it is reported only when
the same file also carries a start or end marker — i.e. only inside an actual
conflict block — keeping the banner case clean.

Scope: every file reported by ``git ls-files`` (tracked content only — never a
build directory or an untracked artifact). Known limitations, deliberately
traded for zero-configuration and zero false positives on this repo:

  * **Marker size.** Only the default seven-character markers are matched. Git's
    ``.gitattributes`` ``conflict-marker-size=N`` produces N-character markers; a
    non-default size would be missed. Not set anywhere in this repo; widening the
    regex would forfeit the zero-false-positive property, so this stays strict.
  * **Submodule contents.** A submodule appears to the superproject as a single
    gitlink, so markers *inside* a submodule's own files are out of scope here —
    they are the submodule's own history to guard.
  * **NUL-bearing text.** UTF-16 (or any file with a NUL in the first 8 KB) is
    skipped as binary, so a conflict in such a file would pass. This repo tracks
    only UTF-8 text.
  * **Separator-only leftovers.** A file hand-resolved down to a bare
    ``=======`` (both arrow markers deleted) is intentionally not flagged, to
    keep Markdown headings and banners clean.

The upstream fix for the whole class is the merge tool refusing to commit a
conflicted result at all (tracked in Shipyard #372); this guard is the
consumer-side backstop.

Usage:
    python3 tools/scripts/conflict_marker_check.py [--root DIR]
    python3 tools/scripts/conflict_marker_check.py --selftest

Exits 0 when no markers are found, 1 on any marker (or a failing self-test),
2 when it cannot enumerate the tree (not a git working tree — an internal error,
distinct from a marker finding).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# ── Marker signatures ──────────────────────────────────────────────────────
# A git conflict block is bounded by a start and an end marker and (in diff3
# mode) a base marker: exactly seven of the character, at column 0, followed by
# whitespace or end-of-line. Matching *exactly* seven (not seven-or-more) is a
# deliberate choice: it excludes decorative rules (a long "<<<<<<<<" divider) so
# the guard stays zero-false-positive, at the cost of missing a non-default
# `conflict-marker-size` (see the module docstring's limitations).
START_END_RE = re.compile(r"^(?:<{7}|>{7}|\|{7})(?:\s|$)")

# The separator is only decisive inside a conflict block — plain runs of "=" are
# common (Markdown headings, banners), so it is reported only when the file also
# has a start/end marker. Matched as a whole line of exactly seven "=".
SEPARATOR_RE = re.compile(r"^={7}$")

# Paths whose tracked content is permitted to contain marker-like lines (e.g. a
# vendored VCS-tooling test fixture). Empty today — growing it is an intentional,
# reviewable act. Compared against the repo-relative POSIX path.
ALLOWLIST: frozenset[str] = frozenset()


def _tracked_files(root: Path) -> list[str] | None:
    """Repo-relative paths of every tracked file, or None if `root` is not a
    git working tree."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]


def _is_binary(data: bytes) -> bool:
    """A NUL byte in the first chunk is git's own heuristic for 'binary'."""
    return b"\0" in data[:8000]


def scan_file(path: Path, rel: str) -> list[str]:
    """Return one message per conflict-marker line found in `path`."""
    try:
        data = path.read_bytes()
    except OSError:
        return []
    if _is_binary(data):
        return []
    text = data.decode("utf-8", "ignore")

    hits: list[tuple[int, str]] = []  # (lineno, kind)
    saw_start_or_end = False
    separators: list[int] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if START_END_RE.match(line):
            hits.append((lineno, "conflict marker"))
            saw_start_or_end = True
        elif SEPARATOR_RE.match(line):
            separators.append(lineno)

    # The bare "=======" separator is only meaningful as part of a real block.
    if saw_start_or_end:
        for lineno in separators:
            hits.append((lineno, "conflict separator"))

    hits.sort()
    return [
        f"{rel}:{lineno}: git {kind} committed — resolve the conflict before "
        "landing this file."
        for lineno, kind in hits
    ]


def run(root: Path) -> list[str]:
    files = _tracked_files(root)
    if files is None:
        return ["conflict-marker: not a git working tree — cannot enumerate files."]
    violations: list[str] = []
    for rel in files:
        if rel in ALLOWLIST:
            continue
        violations.extend(scan_file(root / rel, rel))
    return violations


# ── Self-test ──────────────────────────────────────────────────────────────

# Build markers at runtime so this source file never carries a column-0 marker
# that would trip the guard when it scans its own tracked copy.
_LT = "<" * 7
_GT = ">" * 7
_PIPE = "|" * 7
_EQ = "=" * 7


def selftest() -> int:
    failures = 0

    def case(name: str, files: dict[str, str], expect_hit: bool) -> None:
        nonlocal failures
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            subprocess.run(
                ["git", "-C", str(root), "init", "-q"], check=True
            )
            for rel, content in files.items():
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
            found = run(root)
            got_hit = bool(found)
            ok = got_hit == expect_hit
            if not ok:
                failures += 1
            status = "ok" if ok else "FAIL"
            want = "flagged" if expect_hit else "clean"
            print(f"[{status}] {name} (expect {want}); found={len(found)}")
            if not ok:
                for v in found:
                    print(f"        · {v}")

    conflict = f"a\n{_LT} HEAD\nmine\n{_EQ}\ntheirs\n{_GT} branch\nb\n"
    case("classic conflict block is flagged", {"src.cpp": conflict}, True)

    diff3 = f"{_LT} HEAD\nmine\n{_PIPE} base\norig\n{_EQ}\ntheirs\n{_GT} x\n"
    case("diff3-style conflict is flagged", {"a.txt": diff3}, True)

    case(
        "the VERSION-line incident shape is flagged",
        {"CMakeLists.txt": f"project(pulp\n{_LT} HEAD\n  VERSION 0.558.0\n{_EQ}\n"
                           f"  VERSION 0.557.0\n{_GT} 551acb2b9 (chore: bump versions)\n)\n"},
        True,
    )

    case(
        "clean source passes",
        {"src.cpp": "int main() { return 0; }\n", "README.md": "# Title\n"},
        False,
    )

    # A bare seven-"=" separator with NO start/end marker is a banner/heading,
    # not a conflict — must stay clean.
    case(
        "markdown setext heading is NOT flagged",
        {"doc.md": f"Section Title\n{_EQ}\nbody text\n"},
        False,
    )
    case(
        "ascii banner separator is NOT flagged",
        {"banner.txt": f"{_EQ}\n  PULP  \n{_EQ}\n"},
        False,
    )

    # Eight-or-more runs (decorative rules) must not match the seven-char signal.
    case(
        "long decorative rule is NOT flagged",
        {"art.txt": ("<" * 40) + "\n" + (">" * 40) + "\n"},
        False,
    )

    # An allowlisted path is skipped even with a real marker (escape hatch).
    global ALLOWLIST
    saved = ALLOWLIST
    try:
        ALLOWLIST = frozenset({"external/vendor/fixture.txt"})
        case(
            "allowlisted path is skipped",
            {"external/vendor/fixture.txt": conflict},
            False,
        )
    finally:
        ALLOWLIST = saved

    # Binary files are skipped (a NUL-bearing blob that happens to contain the
    # byte pattern must not be decoded and scanned).
    case(
        "binary file is skipped",
        {"blob.bin": f"\0\0\0{_LT} HEAD\n"},
        False,
    )

    # main() must return 2 (not 1) when handed a non-git directory. The
    # push:main workflow relies on this distinction: exit 1 means "markers
    # committed" (open the tracker), exit 2+ means "internal error" (fail the
    # run WITHOUT opening a factually-wrong marker issue).
    with tempfile.TemporaryDirectory() as td:
        rc = main(["prog", "--root", td])
        ok = rc == 2
        if not ok:
            failures += 1
        print(f"[{'ok' if ok else 'FAIL'}] non-git dir -> exit 2 (not 1); got rc={rc}")

    if failures:
        print(f"\nselftest: {failures} case(s) failed")
        return 1
    print("\nselftest: all cases passed")
    return 0


def main(argv) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=None, help="repo root (default: auto-detect)")
    parser.add_argument("--selftest", action="store_true", help="run fixture self-test")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    root = Path(args.root) if args.root else Path(__file__).resolve().parents[2]
    violations = run(root)
    if violations:
        for v in violations:
            print(v)
        if violations[0].startswith("conflict-marker: not a git"):
            return 2
        print(
            f"\n{len(violations)} git conflict marker(s) found in tracked files.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
