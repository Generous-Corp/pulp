"""Evidence command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable


def cmd_evidence(
    args: argparse.Namespace,
    *,
    current_branch_fn: Callable[[], str],
    evidence_scope_header_line_fn: Callable[[str | None, str | None], str | None],
    print_evidence_summary_fn: Callable[..., bool],
    evidence_empty_line_fn: Callable[..., str],
    print_fn: Callable[[str], None] = print,
) -> int:
    branch = args.branch or current_branch_fn()
    header = evidence_scope_header_line_fn(branch, args.sha)
    if header:
        print_fn(header)

    found = print_evidence_summary_fn(branch=branch, sha=args.sha, limit=args.limit)
    if not found:
        print_fn(evidence_empty_line_fn(has_header=header is not None))
        return 1
    return 0
