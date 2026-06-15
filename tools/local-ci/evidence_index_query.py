"""Evidence index grouping helpers."""

from __future__ import annotations

from collections import defaultdict

from evidence_index_store import load_evidence_index


def collect_evidence_groups_from_index(
    index: dict,
    *,
    branch: str | None = None,
    sha: str | None = None,
) -> dict[str, list[dict]]:
    grouped: dict[str, dict[str, dict]] = defaultdict(dict)

    for record in index.get("entries", {}).values():
        if branch and record.get("branch") != branch:
            continue
        if sha and record.get("sha") != sha:
            continue

        validation = record.get("validation", "full")
        sha_value = record.get("sha", "")
        if not sha_value:
            continue

        bucket = grouped[validation].setdefault(
            sha_value,
            {
                "sha": sha_value,
                "branch": record.get("branch", ""),
                "validation": validation,
                "completed_at": record.get("completed_at", ""),
                "targets": {},
            },
        )
        bucket["targets"][record.get("target", "")] = record
        if record.get("completed_at", "") > bucket.get("completed_at", ""):
            bucket["completed_at"] = record.get("completed_at", "")

    return {
        validation: sorted(
            sha_groups.values(),
            key=lambda item: (item.get("completed_at", ""), item.get("sha", "")),
            reverse=True,
        )
        for validation, sha_groups in grouped.items()
    }


def collect_evidence_groups(branch: str | None = None, sha: str | None = None) -> dict[str, list[dict]]:
    return collect_evidence_groups_from_index(load_evidence_index(), branch=branch, sha=sha)
