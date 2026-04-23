#!/usr/bin/env python3
"""Render a Pulp-flavoured PR comment body from a diff-cover markdown report.

This exists so the workflow step that posts the comment is a simple
`python3 tools/scripts/coverage_diff_comment.py ...` call, and so the
rendering logic is unit-testable without standing up a full CI environment.

Diff-cover produces a markdown report like:

    # Diff Coverage
    ## Diff: origin/main...HEAD, staged and unstaged changes
    - core/audio/src/foo.cpp (82.5%): Lines 10-12 missing
    - core/view/src/bar.cpp (100%)
    ## Summary
    - **Total**: 240 lines
    - **Missing**: 52 lines
    - **Coverage**: 78%

The gate is advisory in Phase 1 (issue #566 Phase 1 PR 3). We want the
comment to:

  1. Always wear a stable HTML marker so the workflow can upsert a single
     comment across re-runs rather than spamming (same pattern as
     .github/workflows/ruleset-drift-check.yml).
  2. Clearly label itself as "advisory — flips blocking on <date>" so
     contributors don't panic when the percentage is red during the
     2-week window.
  3. Fall back gracefully if the diff-cover report is empty (e.g. a PR
     that didn't touch any instrumented source).

Run:
    python3 tools/scripts/coverage_diff_comment.py \
        --report coverage-diff.md \
        --flip-date 2026-05-05 \
        --threshold 75 \
        --out coverage-diff-comment.md

This script never fails CI — it always emits a comment body, even if the
report is missing or malformed. The gate decision is made by diff-cover
itself in the prior step.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Optional


COMMENT_MARKER = "<!-- pulp-coverage-diff-gate -->"


def _read_report(path: pathlib.Path) -> Optional[str]:
    """Return the report contents, or None if it is missing/unreadable.

    We intentionally swallow IOError here — if the diff-cover step failed
    upstream we still want to post a comment explaining what happened so
    contributors aren't left wondering why the percentage box disappeared.
    """
    try:
        return path.read_text(encoding="utf-8")
    except (FileNotFoundError, OSError):
        return None


def render(
    report_text: Optional[str],
    *,
    flip_date: str,
    threshold: int,
    advisory: bool = True,
) -> str:
    """Render the comment body markdown.

    Pure function — no I/O, so the tests can exercise every branch.
    """

    parts: list[str] = []
    parts.append(COMMENT_MARKER)
    parts.append("")
    header = (
        "## Diff coverage (Phase 1 advisory)"
        if advisory
        else "## Diff coverage (required)"
    )
    parts.append(header)
    parts.append("")

    if advisory:
        parts.append(
            f"Advisory window: this gate is **informational only** until "
            f"**{flip_date}**, after which diff coverage below "
            f"**{threshold}%** will block merges (issue #566 Phase 3)."
        )
    else:
        parts.append(
            f"Diff coverage threshold: **{threshold}%** (required)."
        )
    parts.append(
        "This percentage applies only to the coverage surfaces currently "
        "represented on Codecov; see `docs/guides/coverage.md` for the "
        "current perimeter."
    )
    parts.append("")

    if not report_text or not report_text.strip():
        parts.append(
            "_diff-cover produced no report — usually means the PR did not "
            "touch any instrumented source files. Nothing to gate on._"
        )
        parts.append("")
        return "\n".join(parts).rstrip() + "\n"

    # Drop the first "# Diff Coverage" heading if present — the PR comment
    # already has a `##` section heading, and nested `#` inside a collapsed
    # <details> renders oddly in GitHub.
    cleaned = report_text.strip()
    if cleaned.startswith("# "):
        cleaned = "\n".join(cleaned.splitlines()[1:]).lstrip("\n")

    parts.append("<details><summary>diff-cover report</summary>")
    parts.append("")
    parts.append(cleaned)
    parts.append("")
    parts.append("</details>")
    parts.append("")
    parts.append(
        "See [coverage.md](https://github.com/danielraffel/pulp/blob/main/docs/guides/coverage.md) "
        "for the current represented surface, how to interpret this number, "
        "and the full Phase 1 → Phase 3 roadmap."
    )
    parts.append("")
    return "\n".join(parts).rstrip() + "\n"


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--report",
        required=True,
        type=pathlib.Path,
        help="Path to diff-cover --markdown-report output.",
    )
    parser.add_argument(
        "--flip-date",
        required=True,
        help="ISO date (YYYY-MM-DD) after which the gate becomes blocking.",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        required=True,
        help="Percent threshold diff-cover was invoked with.",
    )
    parser.add_argument(
        "--advisory",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Render the advisory banner (default) vs. a required-gate banner.",
    )
    parser.add_argument(
        "--out",
        required=True,
        type=pathlib.Path,
        help="Path to write the rendered comment body markdown.",
    )

    args = parser.parse_args(argv)

    report_text = _read_report(args.report)
    body = render(
        report_text,
        flip_date=args.flip_date,
        threshold=args.threshold,
        advisory=args.advisory,
    )
    args.out.write_text(body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
