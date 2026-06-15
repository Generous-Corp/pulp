"""Evidence index display line helpers."""

from __future__ import annotations

from git_helpers import short_sha
from provenance import provenance_summary
from evidence_index_query import collect_evidence_groups


def print_evidence_summary_from_groups(
    groups: dict[str, list[dict]],
    *,
    limit: int = 3,
    indent: str = "",
) -> bool:
    if not groups:
        return False

    for validation in sorted(groups):
        print(f"{indent}{validation}:")
        for item in groups[validation][:limit]:
            targets = ", ".join(f"{target}=pass" for target in sorted(item.get("targets", {})))
            print(
                f"{indent}  {short_sha(item.get('sha', ''))} [{targets}] "
                f"last={item.get('completed_at', '?')} "
                f"via {provenance_summary(item.get('provenance'))}"
            )
    return True


def print_evidence_summary(
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    return print_evidence_summary_from_groups(
        collect_evidence_groups(branch=branch, sha=sha),
        limit=limit,
        indent=indent,
    )


def evidence_scope_header_line(branch: str | None, sha: str | None) -> str | None:
    if branch:
        return f"Evidence for branch `{branch}`:"
    if sha:
        return f"Evidence for sha `{short_sha(sha)}`:"
    return None


def evidence_empty_line(*, has_header: bool) -> str:
    if has_header:
        return "  (none)"
    return "No local CI evidence recorded."
