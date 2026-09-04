#!/usr/bin/env python3
"""Reject a forced worktree restore used as a repair without a declared reason.

`git checkout --force -- <path>` reads as "put the tracked content back", and it
is the first idiom anyone reaches for when a source cache looks wrong. It does
nothing when git already believes the worktree matches the index: the stat cache
short-circuits the comparison, git restores no bytes, and the command exits 0.
A cache re-normalised by a line-ending config change is exactly that state, so
the repair reports success and leaves the poisoned bytes in place.

The command is not the defect; the reason for reaching for it is. Content git
knows is absent (a partial clone that could not read its blobs) is restored
correctly by a forced checkout, because git does see the difference. Content git
believes is already current must go through `restore_source_cache_verbatim_eol`,
which drops the index first so the next command has to re-materialise every file.

A static scan cannot tell those two apart, so this lint makes the author say
which one it is. Every forced restore under the scanned paths carries

    # forced-restore: content-absent reason="..."

on the line itself or within the three lines above it. `content-absent` is the
only accepted reason; anything else, or nothing at all, points the author at the
sanctioned primitive.

The reason annotation is an escape hatch an author can always write, so it is not
the load-bearing half of this gate. The structural check is: the sanctioned
primitive must still drop the index before re-materialising. That one cannot be
satisfied by a comment, and it is what stops the repair from being quietly
rewritten back into the idiom it replaced.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Worktree-restore idioms: the tree is re-materialised in place, so git compares
# index to worktree and a clean stat cache makes the whole command a no-op.
#
#   checkout --force -- <pathspec>   the canonical form
#   checkout -f -- <pathspec>        the same, abbreviated
#   checkout-index -f -a             writes the index out over the worktree
#   read-tree --reset -u             resets the index and updates the worktree
#
# A forced checkout that also moves the ref (`checkout --force -B main FETCH_HEAD`)
# is deliberately NOT matched: it changes what HEAD points at, so git sees a real
# difference and the stat cache cannot swallow it.
_RESTORE_IDIOMS = (
    re.compile(r"\bcheckout\b(?=[^|;&]*(?:--force|\s-f)\b)[^|;&]*\s--\s"),
    re.compile(r"\bcheckout-index\b[^|;&]*\s-{1,2}(?:f\b|force\b|a\b|all\b)"),
    re.compile(r"\bread-tree\b[^|;&]*--reset\b"),
)

_ANNOTATION = re.compile(r"forced-restore:\s*([a-z][a-z0-9-]*)\s*reason=\"([^\"]+)\"")

_ACCEPTED_REASON = "content-absent"

_SANCTIONED = "restore_source_cache_verbatim_eol"

# Paths the friction report scoped this to: the bootstrap script and the tooling
# around it, which is where a source cache is repaired.
_SCOPE = ("setup.sh", "tools/")

_ANNOTATION_LOOKBACK = 3
# The selftest carries bare restores as fixtures by construction; the sweep
# skips that one file, the detector does not.
_FIXTURES = "tools/scripts/test_forced_restore_lint.py"


def _is_comment(line: str) -> bool:
    """A line that only talks about the idiom is prose, not a repair."""
    stripped = line.lstrip()
    return stripped.startswith("#") or stripped.startswith("//")


def _tracked_files(root: Path) -> list[Path]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--", *_SCOPE],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [root / rel for rel in out.splitlines() if rel]


def _annotation_for(lines: list[str], index: int) -> tuple[str, str] | None:
    """The annotation on the hit line, or within the lines just above it.

    A reason worth writing rarely fits on one line, so the window is flattened
    into a single string with the comment markers dropped before matching.
    """
    start = max(0, index - _ANNOTATION_LOOKBACK)
    window = " ".join(
        line.lstrip().lstrip("#").lstrip("/").strip() for line in lines[start : index + 1]
    )
    found = _ANNOTATION.search(window)
    return (found.group(1), found.group(2)) if found else None


def _docstring_lines(lines: list[str]) -> set[int]:
    """Indices of lines inside a Python triple-quoted string.

    Prose has to be able to quote the trap without tripping the gate — this
    file's own docstring names the idiom, and so does any doc that explains
    why the idiom is wrong. Shell has no equivalent construct, so this only
    ever applies to `.py` sources, where a bare command line cannot appear
    inside a docstring anyway.
    """
    inside: set[int] = set()
    delimiter: str | None = None
    for i, line in enumerate(lines):
        if delimiter is None:
            for quote in ('"""', "'''"):
                # Only a line that *opens* with the quote is a docstring. An
                # inline triple-quoted argument — run("""git ...""") — is a
                # live command, and exempting it would be the hole this whole
                # gate exists to close.
                if line.lstrip().startswith(quote):
                    # The line itself is prose either way. An odd count leaves
                    # the string open, so it also starts a region; an even one
                    # (a one-line docstring) opens and closes in place.
                    inside.add(i)
                    if line.count(quote) % 2 == 1:
                        delimiter = quote
                    break
        else:
            inside.add(i)
            if delimiter in line:
                delimiter = None
    return inside


def scan_text(text: str, path: str) -> list[str]:
    """Violations in one file's text, as human-readable lines."""
    violations: list[str] = []
    lines = text.splitlines()
    prose = _docstring_lines(lines) if path.endswith(".py") else set()
    for i, line in enumerate(lines):
        if i in prose or _is_comment(line):
            continue
        if not any(idiom.search(line) for idiom in _RESTORE_IDIOMS):
            continue
        annotation = _annotation_for(lines, i)
        if annotation is None:
            violations.append(
                f"{path}:{i + 1}: forced worktree restore with no declared reason.\n"
                f"    {line.strip()}\n"
                f"    This is a no-op when git believes the worktree is already current,\n"
                f"    which is exactly the state a re-normalised cache is in. If the content\n"
                f"    is genuinely absent, say so:\n"
                f'        # forced-restore: content-absent reason="..."\n'
                f"    If it may only be stale or converted, repair it through {_SANCTIONED}(),\n"
                f"    which drops the index first."
            )
            continue
        reason_kind, _ = annotation
        if reason_kind != _ACCEPTED_REASON:
            violations.append(
                f"{path}:{i + 1}: forced worktree restore declared '{reason_kind}'.\n"
                f"    {line.strip()}\n"
                f"    '{_ACCEPTED_REASON}' is the only state a forced restore repairs.\n"
                f"    Anything git may already believe is current must go through\n"
                f"    {_SANCTIONED}()."
            )
    return violations


def check_primitive(setup_text: str) -> list[str]:
    """The sanctioned primitive must still drop the index before restoring.

    An annotation is something an author can always write. This is not: if the
    primitive is ever simplified back into a plain restore, it stops working and
    every caller silently inherits the original defect.
    """
    match = re.search(
        rf"^{_SANCTIONED}\(\) \{{\n(.*?)^\}}",
        setup_text,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        return [
            f"setup.sh: {_SANCTIONED}() is gone. It is the sanctioned repair for a\n"
            f"    cache git believes is already current; removing it leaves callers with\n"
            f"    the forced-restore idiom, which does nothing in that state."
        ]
    body = match.group(1)
    problems: list[str] = []
    if "rev-parse --git-path index" not in body:
        problems.append(
            f"setup.sh: {_SANCTIONED}() no longer resolves the index path.\n"
            f"    It must delete the index, and it cannot assume .git/index: a worktree\n"
            f"    or a submodule keeps its index somewhere else."
        )
    if not re.search(r"\brm -f\b", body):
        problems.append(
            f"setup.sh: {_SANCTIONED}() no longer deletes the index.\n"
            f"    Dropping the index is the whole mechanism: it is what forces the next\n"
            f"    command to re-materialise files git would otherwise call current."
        )
    if not re.search(r"\breset --hard\b", body):
        problems.append(
            f"setup.sh: {_SANCTIONED}() no longer re-materialises the worktree.\n"
            f"    Deleting the index without rebuilding it leaves the cache unusable."
        )
    return problems


def run(root: Path) -> int:
    violations: list[str] = []
    for path in _tracked_files(root):
        if str(path.relative_to(root)) == _FIXTURES:
            # The selftest holds bare restores as fixtures — that is its whole
            # job. Exempting it here keeps the sweep quiet without softening
            # scan_text(), which still fires on every one of those fixtures.
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        violations += scan_text(text, str(path.relative_to(root)))

    setup = root / "setup.sh"
    if setup.is_file():
        violations += check_primitive(setup.read_text(encoding="utf-8"))

    if violations:
        print("forced-restore lint: FAILED", file=sys.stderr)
        for violation in violations:
            print(f"\n{violation}", file=sys.stderr)
        return 1
    print("forced_restore_lint: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=None,
        help="repository root to scan (default: the repository this script is in)",
    )
    args = parser.parse_args()
    root = (
        Path(args.root).resolve()
        if args.root
        else Path(
            subprocess.run(
                ["git", "-C", str(Path(__file__).resolve().parent), "rev-parse", "--show-toplevel"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        )
    )
    return run(root)


if __name__ == "__main__":
    sys.exit(main())
