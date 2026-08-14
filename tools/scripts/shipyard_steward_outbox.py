#!/usr/bin/env python3
"""Render Shipyard's durable GitHub PR exception outbox.

The steward report is authoritative for its decision. The REST pull census is
used only to make that decision understandable to a human (title, age, author,
and provenance labels). This script performs no network or GitHub mutation.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ISSUE_TITLE = "[Shipyard steward] Pulp PR exception outbox"
NORMAL_ACTIONS = {
    "arm_merge_queue",
    "draft",
    "opted_out",
    "queued",
    "rerun_transient",
    "waiting_required",
}


@dataclass(frozen=True)
class ExceptionRow:
    number: int
    title: str
    head_sha: str
    action: str
    reason: str
    author: str
    age_days: int
    updated_days: int
    labels: tuple[str, ...]


def _load(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _parse_time(value: str) -> datetime:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def _age_days(now: datetime, value: str) -> int:
    return max(0, int((now - _parse_time(value)).total_seconds() // 86_400))


def _one_line(value: object) -> str:
    text = " ".join(str(value or "").split())
    return (
        text.replace("\\", "\\\\")
        .replace("[", "\\[")
        .replace("]", "\\]")
        .replace("|", "\\|")
        .replace("@", "@\u200b")
    )


def _pull_rows(raw: Any) -> list[dict[str, Any]]:
    if not isinstance(raw, list):
        raise ValueError("pull census must be an array")
    rows: list[dict[str, Any]] = []
    for item in raw:
        if isinstance(item, list):
            rows.extend(value for value in item if isinstance(value, dict))
        elif isinstance(item, dict):
            rows.append(item)
    return rows


def _decision(pr: dict[str, Any]) -> tuple[str, str]:
    decision = pr.get("decision")
    if not isinstance(decision, dict):
        return "invalid_report", "missing structured steward decision"
    action = str(decision.get("action") or "invalid_report")
    if action == "required_failed":
        contexts = ", ".join(map(str, decision.get("contexts") or []))
        return action, f"required checks failed: {contexts or 'unknown'}"
    if action == "needs_update":
        return action, f"merge state {decision.get('merge_state') or 'unknown'}"
    if action == "handoff_missing":
        return action, "managed label exists but current exact-head receipt is missing"
    if action == "unmanaged":
        return action, "no explicit exact-head Shipyard handoff"
    if action == "invalid_head":
        return action, "GitHub returned an invalid or unreadable PR head"
    if action == "direct_merge_refused":
        reasons = ", ".join(map(str, decision.get("reasons") or []))
        return action, f"server-owned merge enforcement unavailable: {reasons or 'unknown'}"
    return action, action.replace("_", " ")


def _pull_row(
    number: int,
    pull: dict[str, Any],
    now: datetime,
    *,
    head_sha: str,
    action: str,
    reason: str,
) -> ExceptionRow:
    labels = tuple(
        sorted(
            str(label.get("name"))
            for label in pull.get("labels") or []
            if isinstance(label, dict) and label.get("name")
        )
    )
    created = str(pull.get("created_at") or now.isoformat())
    updated = str(pull.get("updated_at") or created)
    author = pull.get("user") if isinstance(pull.get("user"), dict) else {}
    return ExceptionRow(
        number=number,
        title=str(pull.get("title") or "(title unavailable)"),
        head_sha=head_sha,
        action=action,
        reason=reason,
        author=str(author.get("login") or "unknown"),
        age_days=_age_days(now, created),
        updated_days=_age_days(now, updated),
        labels=labels,
    )


def build_outbox(
    report: dict[str, Any], pulls_raw: Any, now: datetime
) -> tuple[list[ExceptionRow], list[str], str]:
    repos = report.get("repos")
    if not isinstance(repos, list) or len(repos) != 1 or not isinstance(repos[0], dict):
        raise ValueError("steward report must contain exactly one repository")
    repo = repos[0]
    repo_slug = str(repo.get("repo") or "")
    if not repo_slug or "/" not in repo_slug:
        raise ValueError("steward report repository is missing or malformed")
    managed_base = str(repo.get("base") or "")

    pulls = {
        int(pull["number"]): pull
        for pull in _pull_rows(pulls_raw)
        if isinstance(pull.get("number"), int)
    }
    rows: list[ExceptionRow] = []
    observed_numbers: set[int] = set()
    for pr in repo.get("prs") or []:
        if not isinstance(pr, dict) or not isinstance(pr.get("number"), int):
            continue
        action, reason = _decision(pr)
        error = pr.get("error")
        if error:
            action = "mutation_error"
            reason = str(error)
        if action in NORMAL_ACTIONS:
            observed_numbers.add(int(pr["number"]))
            continue
        number = int(pr["number"])
        observed_numbers.add(number)
        pull = pulls.get(number, {})
        rows.append(
            _pull_row(
                number,
                pull,
                now,
                head_sha=str(pr.get("head_sha") or ""),
                action=action,
                reason=reason,
            )
        )
    for number, pull in pulls.items():
        if number in observed_numbers:
            continue
        base = pull.get("base") if isinstance(pull.get("base"), dict) else {}
        head = pull.get("head") if isinstance(pull.get("head"), dict) else {}
        base_ref = str(base.get("ref") or "unknown")
        outside_scope = not managed_base or base_ref != managed_base
        rows.append(
            _pull_row(
                number,
                pull,
                now,
                head_sha=str(head.get("sha") or ""),
                action="outside_scope" if outside_scope else "census_gap",
                reason=(
                    f"open PR targets {base_ref}; the "
                    f"{managed_base or 'configured'} steward does not manage that base"
                    if outside_scope
                    else f"open PR targets managed base {managed_base} but "
                    "appeared after or outside the steward snapshot"
                ),
            )
        )
    rows.sort(key=lambda row: (-row.age_days, row.number))
    errors = [str(error) for error in repo.get("errors") or []]
    return rows, errors, repo_slug


def render_markdown(
    rows: list[ExceptionRow], errors: list[str], repo_slug: str, now: datetime
) -> str:
    generated = now.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    lines = [
        "<!-- shipyard-steward-outbox:v1 -->",
        f"# {ISSUE_TITLE}",
        "",
        f"Last reconciled: `{generated}` by the model-free GitHub-hosted controller.",
        "",
        "GitHub and Shipyard are authoritative here. Linear may hold the "
        "semantic work item, but this issue remains usable when Linear or "
        "every Mac is offline.",
        "",
        f"- PR exceptions: **{len(rows)}**",
        f"- Control-plane errors: **{len(errors)}**",
        "",
    ]
    if errors:
        lines.extend(["## Control-plane errors", ""])
        lines.extend(f"- `{_one_line(error)}`" for error in errors)
        lines.append("")
    if rows:
        lines.extend(
            [
                "## PR exceptions",
                "",
                "| PR | Exact head | Age / idle | Provenance labels | Reason | Next action |",
                "| --- | --- | --- | --- | --- | --- |",
            ]
        )
        for row in rows:
            pr_link = (
                f"[#{row.number} {_one_line(row.title)}]"
                f"(https://github.com/{repo_slug}/pull/{row.number})"
            )
            labels = ", ".join(f"`{_one_line(label)}`" for label in row.labels) or "none"
            next_action = (
                "Fix from the exact head, then let Shipyard clear `needs-agent`."
                if row.action in {"required_failed", "needs_update", "mutation_error"}
                else "Retarget into a managed landing chain, explicitly govern that base, or close."
                if row.action == "outside_scope"
                else "Leave untouched; the next tick must observe it, or the "
                "repeated census gap needs repair."
                if row.action == "census_gap"
                else "Adopt with an exact-head handoff, or explicitly opt out/close."
            )
            lines.append(" | ".join((
                f"| {pr_link}",
                f"`{_one_line(row.head_sha[:12])}`",
                f"{row.age_days}d / {row.updated_days}d",
                labels,
                f"`{_one_line(row.action)}`: {_one_line(row.reason)}",
                f"{next_action} |",
            )))
        lines.append("")
    else:
        lines.extend(
            [
                "No PR currently requires exception handling. Managed PRs "
                "that are normally waiting, queued, or mergeable are "
                "intentionally omitted.",
                "",
            ]
        )
    lines.extend(
        [
            "## Contract",
            "",
            "- `unmanaged` is visible but never adopted implicitly.",
            "- Every GitHub-open PR outside the managed base is retained as `outside_scope`.",
            "- A PR created between snapshots remains visible as `census_gap` "
            "and is never mutated from stale facts.",
            "- `shipyard:needs-agent` is deduplicated by exact head and blocker state.",
            "- Routine checks, retry classification, queue admission, merge "
            "confirmation, and cleanup invoke no model.",
            "- This issue is updated in place; it closes when the exception "
            "count reaches zero and reopens on recurrence.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--pulls", type=Path, required=True)
    parser.add_argument("--body", type=Path, required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--now", help="RFC3339 test override")
    args = parser.parse_args()

    now = _parse_time(args.now) if args.now else datetime.now(timezone.utc)
    rows, errors, repo_slug = build_outbox(_load(args.report), _load(args.pulls), now)
    args.body.write_text(render_markdown(rows, errors, repo_slug, now), encoding="utf-8")
    args.state.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "issue_title": ISSUE_TITLE,
                "exception_count": len(rows) + len(errors),
                "pr_exception_count": len(rows),
                "control_plane_error_count": len(errors),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
