#!/usr/bin/env python3
"""Parse include-what-you-use output and emit GitHub annotations.

IWYU prints per-file suggestions like:

    core/signal/include/pulp/signal/fft.hpp should add these lines:
    #include <memory>                      // for unique_ptr

    core/signal/include/pulp/signal/fft.hpp should remove these lines:
    - #include <vector>  // lines 3-3

    The full include-list for core/signal/include/pulp/signal/fft.hpp:
    ...
    ---

Issue #594 only cares about the "should add" section — that is
the class of bug (transitive-include) that keeps reaching main. "Should
remove" is the minimal-include class of suggestion IWYU is famous for,
and #594 explicitly calls it a non-goal.

This script:

  1. Reads IWYU output from stdin (or --input <path>).
  2. Walks the per-file blocks.
  3. For every "should add" line, emits:

        ::warning file=<path>,line=1,title=IWYU::<file> should add <header> <reason>

  4. Optionally filters to a set of changed files passed via
     --changed-files-from <path> (one path per line, typically the
     output of `git diff --name-only origin/main...HEAD`).

  5. Writes a short summary block to stdout (so the CI log is readable)
     and optionally to $GITHUB_STEP_SUMMARY if --summary-file is given.

The script never fails — it always exits 0. The gate decision is
upstream (continue-on-error in the workflow). That keeps the script
drop-in usable in either advisory or blocking mode: flip the workflow
`continue-on-error: false` and the matching annotations will still fire.

Run:
    iwyu_tool.py -p build ... | python3 tools/scripts/iwyu_annotate.py \\
        --changed-files-from changed.txt \\
        --summary-file "$GITHUB_STEP_SUMMARY"
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Iterable, Iterator, List, Optional, Sequence, Tuple


# ── IWYU block parser ──────────────────────────────────────────────────

_HEADER_RE = re.compile(r"^(?P<path>\S[^\n]*?) should add these lines:\s*$")
_ADD_LINE_RE = re.compile(
    r"""^
        \#include\s+
        (?P<include>[<\"][^>\"]+[>\"])
        (?:\s*//\s*(?P<reason>.+))?
        \s*$
    """,
    re.VERBOSE,
)
_TERMINATOR_RE = re.compile(r"^\s*(?:---|The full include-list for )")


def iter_add_findings(lines: Iterable[str]) -> Iterator[Tuple[str, str, str]]:
    """Yield (file_path, include, reason) for every "should add" entry.

    The IWYU output block structure is:

        <path> should add these lines:
        #include <X>     // for foo
        #include "Y.h"
        <blank line>
        <path> should remove these lines:
        ...

    We walk until we hit an empty/non-matching line, then drop back into
    scanning for the next header.
    """

    in_add_block = False
    current_file: Optional[str] = None

    for raw in lines:
        line = raw.rstrip("\n")

        if in_add_block:
            if not line.strip():
                in_add_block = False
                current_file = None
                continue
            if _TERMINATOR_RE.match(line):
                in_add_block = False
                current_file = None
                continue
            m = _ADD_LINE_RE.match(line.strip())
            if m and current_file is not None:
                include = m.group("include")
                reason = (m.group("reason") or "").strip()
                yield current_file, include, reason
            # else: IWYU sometimes annotates with namespace/ctor hints
            # on subsequent lines — ignore them; the "for <symbol>" on
            # the comment of the previous line is enough context.
            continue

        header = _HEADER_RE.match(line)
        if header:
            in_add_block = True
            current_file = header.group("path").strip()


# ── Changed-file filter ────────────────────────────────────────────────

def load_changed_files(path: Optional[pathlib.Path]) -> Optional[set[str]]:
    """Return the set of paths to filter annotations to, or None to skip."""
    if path is None:
        return None
    try:
        text = path.read_text(encoding="utf-8")
    except (FileNotFoundError, OSError):
        return set()
    changed: set[str] = set()
    for row in text.splitlines():
        stripped = row.strip()
        if not stripped:
            continue
        # Normalize — IWYU prints repo-relative paths just like `git diff`.
        changed.add(stripped)
    return changed


def is_relevant(path: str) -> bool:
    """Filter out noisy paths that IWYU scans but we don't enforce."""
    # Only C++ source / headers.
    if not path.endswith((".cpp", ".cc", ".cxx", ".hpp", ".h", ".hh", ".hxx", ".mm", ".m")):
        return False
    # Skip generated / vendored trees.
    skip_prefixes = (
        "build/",
        "build-coverage/",
        "external/",
        "_deps/",
    )
    norm = path.replace("\\", "/")
    if any(norm.startswith(p) or f"/{p}" in norm for p in skip_prefixes):
        return False
    return True


# ── Rendering ──────────────────────────────────────────────────────────

def render_annotation(file_path: str, include: str, reason: str) -> str:
    """Render a GitHub `::warning` annotation line.

    GitHub's annotation parser strips newlines in the `title=` field, so
    we keep it single-line. We anchor the annotation to line 1 of the
    file rather than trying to guess the right line — IWYU doesn't emit
    a target line for additions (they haven't happened yet).
    """
    title = "IWYU: missing include (advisory, issue #594)"
    msg_parts = [f"add `{include}`"]
    if reason:
        msg_parts.append(reason)
    message = " — ".join(msg_parts)
    # Escape commas and colons in the message per GitHub workflow commands.
    safe_message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    return f"::warning file={file_path},line=1,title={title}::{safe_message}"


def render_summary(findings: Sequence[Tuple[str, str, str]], *, advisory: bool, flip_date: str) -> str:
    """Render the human-readable summary markdown."""
    lines: List[str] = []
    lines.append("## IWYU advisory report (issue #594 Phase 2)")
    lines.append("")
    if advisory:
        lines.append(
            f"This check is **advisory** until **{flip_date}**. "
            "Findings annotate inline on the PR diff but do not block merge. "
            "See [docs/guides/iwyu.md](../docs/guides/iwyu.md)."
        )
        lines.append("")
    if not findings:
        lines.append("No missing-include suggestions on the changed files.")
        return "\n".join(lines) + "\n"

    by_file: dict[str, list[tuple[str, str]]] = {}
    for fp, inc, reason in findings:
        by_file.setdefault(fp, []).append((inc, reason))

    lines.append(f"Found **{len(findings)}** missing-include suggestion(s) across **{len(by_file)}** file(s).")
    lines.append("")
    for fp in sorted(by_file):
        lines.append(f"- `{fp}`")
        for inc, reason in by_file[fp]:
            if reason:
                lines.append(f"  - add `{inc}` — {reason}")
            else:
                lines.append(f"  - add `{inc}`")
    lines.append("")
    lines.append(
        "Not every finding is real: IWYU has known false positives on "
        "CHOC amalgamated headers and libc++/libstdc++ detail paths. "
        "Document recurring FPs in `.iwyu-mappings.imp`."
    )
    return "\n".join(lines) + "\n"


# ── Entry point ────────────────────────────────────────────────────────

def run(
    input_stream: Iterable[str],
    *,
    changed_files: Optional[set[str]],
    advisory: bool,
    flip_date: str,
) -> Tuple[List[str], str]:
    """Pure function: parse IWYU output, return (annotations, summary)."""
    findings: List[Tuple[str, str, str]] = []
    for file_path, include, reason in iter_add_findings(input_stream):
        if not is_relevant(file_path):
            continue
        if changed_files is not None and file_path not in changed_files:
            continue
        findings.append((file_path, include, reason))

    annotations = [render_annotation(fp, inc, reason) for fp, inc, reason in findings]
    summary = render_summary(findings, advisory=advisory, flip_date=flip_date)
    return annotations, summary


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, default=None,
                        help="Read IWYU output from path (default: stdin)")
    parser.add_argument("--changed-files-from", type=pathlib.Path, default=None,
                        help="File with one path per line; filter annotations to these paths.")
    parser.add_argument("--summary-file", type=pathlib.Path, default=None,
                        help="Path to append the human-readable summary (e.g. $GITHUB_STEP_SUMMARY).")
    parser.add_argument("--advisory", action="store_true", default=True,
                        help="Label the summary as advisory (default).")
    parser.add_argument("--blocking", dest="advisory", action="store_false",
                        help="Label the summary as blocking instead of advisory.")
    parser.add_argument("--flip-date", default="2026-05-05",
                        help="Planned flip-to-blocking date, shown in summary.")
    args = parser.parse_args(argv)

    if args.input is not None:
        input_text = args.input.read_text(encoding="utf-8")
        input_iter: Iterable[str] = input_text.splitlines()
    else:
        input_iter = sys.stdin

    changed_files = load_changed_files(args.changed_files_from)

    annotations, summary = run(
        input_iter,
        changed_files=changed_files,
        advisory=args.advisory,
        flip_date=args.flip_date,
    )

    # Annotations go to stdout so the GitHub Actions log parser picks them up.
    for line in annotations:
        print(line)

    # Summary goes to stderr (readable) + optional summary-file.
    sys.stderr.write("\n")
    sys.stderr.write(summary)
    sys.stderr.write("\n")
    if args.summary_file is not None:
        try:
            with args.summary_file.open("a", encoding="utf-8") as fh:
                fh.write(summary)
        except OSError as exc:
            sys.stderr.write(f"warning: could not write summary: {exc}\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
