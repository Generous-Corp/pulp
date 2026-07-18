#!/usr/bin/env python3
"""Detect the "green PRs, but nothing is merging" stall — a wedged auto-merger.

The failure this catches
------------------------
Every required check can be green, every PR can be mergeable, nothing can be
queued waiting on a runner — and still nothing merges, for hours, because the
component that actually presses the merge button (Shipyard's per-host
queue-tick, or a future GitHub merge queue) is silently held in a state where it
reaps but never advances. The queue-age watchdog (runner-health-check.yml) does
NOT see this: that guard alarms on jobs sitting *queued* because a runner lane
died. This is the opposite shape — checks GREEN, nothing queued, yet the merge
throughput is zero. No job-level signal exists for "everything is green and
nobody is merging"; the only observable is a population of merge-ready PRs that
stays merge-ready and unmerged.

The alarm predicate (per open PR)
---------------------------------
A PR is "merge-ready and stuck" this tick when ALL hold:

1. **Required checks green.** Every check in the repo's REQUIRED set (read from
   branch protection at runtime, not hardcoded — see ``resolve_required_checks``)
   has concluded successfully. A missing or pending required check is not green,
   so it is not stuck; it is just not ready.
2. **mergeStateStatus in {CLEAN, BEHIND}.** GitHub's own merge verdict. CLEAN =
   ready to merge; BEHIND = ready but the base moved (an auto-merger updates and
   merges it). DIRTY (conflicts), BLOCKED (a required check red/missing/review
   pending), and UNSTABLE (a non-required check still moving) are all correctly
   excluded — those are waiting on something real, not on a wedged merger.
3. **Auto-merge / queue eligible.** Auto-merge is enabled on the PR (the signal
   that a machine, not a human, owns pressing merge). A green PR with no
   auto-merge is waiting on a person and must not alarm.
4. **Merge-ready age past the threshold.** The PR has been green for longer than
   ``--threshold-minutes`` (default 45), measured from the completion time of
   the last required check to go green — a real duration, independent of this
   watchdog's own cadence.

Why "two consecutive ticks"
---------------------------
A single snapshot can misread. A per-PR REST poll of merge state gets
rate-limited and returns false CLEAN/BEHIND readings under load — that literally
happened during the incident this guard is built for, which is why collection
uses one GraphQL call for every open PR's state instead. Belt-and-suspenders on
top of that: a PR must satisfy the full predicate on TWO consecutive sweeps
before it is issue-worthy. The first qualifying sweep records it as *pending*
(run-summary only); the second consecutive sweep promotes it to *alarm*. A
normal in-flight PR that merges within a tick never reaches the second
observation, so it never trips. The cross-tick memory is the set of PR numbers
that qualified last sweep, persisted as a workflow artifact (crash-safe: GitHub
holds it independently of this repo or any host).

``analyze()`` is pure: snapshot + previous-tick set + now -> (findings, this-tick
set). ``--snapshot`` / ``--prev-state`` feed recorded inputs (tests, replay); the
default path collects a live snapshot via ``gh api graphql``.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from typing import Any

THRESHOLD_MINUTES = 45

# Fallback only. The workflow reads the live required-check set from branch
# protection; this list is what we assume when that read is unavailable (the
# default GITHUB_TOKEN cannot always see protection rules). Kept in sync with
# the documented required set for the `main` branch.
DEFAULT_REQUIRED_CHECKS = ["macos", "Enforce version & skill sync"]

# A CheckRun conclusion that satisfies a required-status-check gate. SKIPPED and
# NEUTRAL count as green to branch protection, so they count here too.
GREEN_CONCLUSIONS = {"SUCCESS", "SKIPPED", "NEUTRAL"}

# The merge verdicts that mean "ready; only the merger has to act."
READY_MERGE_STATES = {"CLEAN", "BEHIND"}


def parse_ts(value: str) -> dt.datetime:
    """Parse a GitHub ISO-8601 timestamp into an aware UTC datetime."""
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    stamp = dt.datetime.fromisoformat(text)
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=dt.timezone.utc)
    return stamp.astimezone(dt.timezone.utc)


def _minutes_between(later: dt.datetime, earlier: dt.datetime) -> float:
    return (later - earlier).total_seconds() / 60.0


def _required_check_state(
    checks: dict[str, dict[str, Any]], required: set[str]
) -> tuple[bool, dt.datetime | None, list[str]]:
    """Return (all_green, green_since, missing_or_pending).

    ``green_since`` is the completion time of the LAST required check to go
    green — the moment the PR became fully green — or None if any required
    check is not green.
    """
    missing: list[str] = []
    completions: list[dt.datetime] = []
    for name in sorted(required):
        entry = checks.get(name)
        if not entry or not entry.get("green"):
            missing.append(name)
            continue
        completed = entry.get("completed_at")
        if completed:
            completions.append(parse_ts(completed))
    if missing:
        return False, None, missing
    green_since = max(completions) if completions else None
    return True, green_since, []


def analyze(
    snapshot: dict[str, Any],
    prev_stuck: list[int] | set[int] | None,
    now: dt.datetime,
    threshold_minutes: float = THRESHOLD_MINUTES,
) -> tuple[list[dict[str, Any]], list[int]]:
    """Return (findings, stuck_now).

    ``findings`` carry ``level``: "alarm" (merge-ready and stuck for two
    consecutive sweeps — issue-worthy) or "pending" (qualified this sweep for
    the first time — run-summary only, promoted next sweep if it persists).

    ``stuck_now`` is the set of PR numbers that satisfy the full predicate
    THIS sweep; it becomes the next sweep's ``prev_stuck`` so a second
    consecutive qualification can promote pending -> alarm.
    """
    required = set(snapshot.get("required_checks") or DEFAULT_REQUIRED_CHECKS)
    required_source = snapshot.get("required_checks_source", "default")
    prev = {int(x) for x in (prev_stuck or [])}

    findings: list[dict[str, Any]] = []
    stuck_now: list[int] = []

    for pr in snapshot.get("open_prs", []):
        number = int(pr["number"])

        # A draft is not a merge candidate; a person is still editing it.
        if pr.get("is_draft"):
            continue
        # Auto-merge (or a merge-queue enqueue) is the "a machine owns the
        # merge" signal. A green PR without it is waiting on a human.
        if not pr.get("auto_merge_enabled"):
            continue
        if pr.get("merge_state_status") not in READY_MERGE_STATES:
            continue

        checks = pr.get("checks", {})
        all_green, green_since, missing = _required_check_state(checks, required)
        if not all_green:
            continue

        # Merge-ready age from the check clock, not this watchdog's cadence.
        # If GitHub reported no completion timestamp for any required check
        # (unusual), we cannot prove the age, so we do not alarm this sweep.
        if green_since is None:
            continue
        age = _minutes_between(now, green_since)
        if age < threshold_minutes:
            continue

        # Full predicate satisfied this sweep.
        stuck_now.append(number)
        level = "alarm" if number in prev else "pending"
        findings.append(
            {
                "level": level,
                "number": number,
                "title": pr.get("title", ""),
                "url": pr.get("url", ""),
                "merge_state_status": pr.get("merge_state_status"),
                "ready_minutes": round(age, 1),
                "green_since": green_since.isoformat().replace("+00:00", "Z"),
                "required_checks": sorted(required),
                "required_checks_source": required_source,
            }
        )

    # Alarms first, then longest-stuck first.
    findings.sort(key=lambda f: (f["level"] != "alarm", -f["ready_minutes"]))
    stuck_now.sort()
    return findings, stuck_now


# --------------------------------------------------------------------------
# FUTURE / TODO — merge-queue stall predicate (DO NOT IMPLEMENT YET)
# --------------------------------------------------------------------------
def merge_queue_stall_todo(snapshot: dict[str, Any], now: dt.datetime) -> list[dict[str, Any]]:
    """STUB — enable ONLY after the GitHub merge queue is turned on for `main`.

    The predicate above catches a wedged AUTO-MERGER (Shipyard's queue-tick held
    in reap-only mode): green PRs that stay green and unmerged. Once merges route
    through a GitHub *merge queue* instead, a different wedge becomes possible —
    the queue itself stalls: entries are enqueued but the queue stops forming the
    `merge_group` batches that actually test-and-merge them. That shape is
    invisible to the predicate above, because an enqueued PR's own
    mergeStateStatus is no longer CLEAN/BEHIND while it waits in the queue.

    Second condition to add THEN (not now — the queue is not live):

        merge queue depth > 0  AND  no `merge_group` check run has STARTED in the
        last 30 minutes

    i.e. work is enqueued but the queue has formed no batch in a full sweep-plus
    window. Data source: the merge-queue GraphQL fields (`MergeQueue.entries`,
    their `enqueuedAt`) plus `merge_group`-triggered check runs on the base
    branch. Until the queue is enabled this returns nothing and is never called.
    """
    return []  # intentionally inert until the merge queue is enabled


# --------------------------------------------------------------------------
# Live collection
# --------------------------------------------------------------------------


def _gh(args: list[str]) -> str:
    # In Actions this is `gh` on GITHUB_TOKEN. Locally, PULP_GH_BIN=ghapp routes
    # through the Shipyard GitHub App's own rate-limit bucket.
    gh_bin = os.environ.get("PULP_GH_BIN") or "gh"
    proc = subprocess.run(
        [gh_bin, *args], capture_output=True, text=True, check=True
    )
    return proc.stdout


_PR_QUERY = """
query($owner:String!, $name:String!, $cursor:String) {
  repository(owner:$owner, name:$name) {
    pullRequests(states: OPEN, first: 50, after: $cursor,
                 orderBy: {field: CREATED_AT, direction: ASC}) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        title
        url
        isDraft
        mergeStateStatus
        autoMergeRequest { enabledAt }
        commits(last: 1) {
          nodes {
            commit {
              statusCheckRollup {
                contexts(first: 100) {
                  nodes {
                    __typename
                    ... on CheckRun {
                      name
                      status
                      conclusion
                      completedAt
                    }
                    ... on StatusContext {
                      context
                      state
                      createdAt
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}
"""


def _checks_from_rollup(commit: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Flatten a statusCheckRollup into name -> {green, completed_at}."""
    checks: dict[str, dict[str, Any]] = {}
    rollup = (commit or {}).get("statusCheckRollup") or {}
    for ctx in (rollup.get("contexts") or {}).get("nodes", []) or []:
        typename = ctx.get("__typename")
        if typename == "CheckRun":
            name = ctx.get("name")
            green = ctx.get("status") == "COMPLETED" and ctx.get(
                "conclusion"
            ) in GREEN_CONCLUSIONS
            completed = ctx.get("completedAt")
        elif typename == "StatusContext":
            name = ctx.get("context")
            green = ctx.get("state") == "SUCCESS"
            # Legacy status contexts have no completion time; createdAt is the
            # only timestamp available and is a safe lower bound on "green since".
            completed = ctx.get("createdAt")
        else:
            continue
        if not name:
            continue
        # A required gate may report multiple contexts of the same name across a
        # matrix; keep the one that is green with the latest completion so
        # green_since reflects the last leg to finish.
        prior = checks.get(name)
        if prior is None or (green and not prior["green"]):
            checks[name] = {"green": green, "completed_at": completed}
        elif green and prior["green"]:
            if completed and (
                not prior["completed_at"] or completed > prior["completed_at"]
            ):
                checks[name]["completed_at"] = completed
    return checks


def resolve_required_checks(repo: str, base: str) -> tuple[list[str], str]:
    """Read the required status checks for ``base`` from branch protection.

    Returns (checks, source) where source is "branch-protection" on a live read
    or "default" on the documented fallback. The default GITHUB_TOKEN often
    cannot read protection rules (needs admin), so a failure here is expected and
    must degrade to the documented set rather than fail the sweep.
    """
    try:
        raw = _gh(
            [
                "api",
                "-H",
                "Accept: application/vnd.github+json",
                f"/repos/{repo}/branches/{base}/protection/required_status_checks",
            ]
        )
        data = json.loads(raw)
        contexts = data.get("contexts") or [
            c.get("context") for c in (data.get("checks") or [])
        ]
        contexts = [c for c in contexts if c]
        if contexts:
            return sorted(set(contexts)), "branch-protection"
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError):
        pass
    return list(DEFAULT_REQUIRED_CHECKS), "default"


def collect_snapshot(repo: str, base: str, now: dt.datetime) -> dict[str, Any]:
    """Collect all open PRs' merge state via one paginated GraphQL query.

    GraphQL, not a per-PR REST loop: a REST poll of each PR's mergeable state
    gets rate-limited under load and returns false readings (the incident that
    motivated this guard). One GraphQL call returns every open PR's
    mergeStateStatus reliably.
    """
    owner, _, name = repo.partition("/")
    required, required_source = resolve_required_checks(repo, base)
    snapshot: dict[str, Any] = {
        "generated_at": now.isoformat(),
        "repo": repo,
        "base": base,
        "required_checks": required,
        "required_checks_source": required_source,
        "open_prs": [],
        "errors": [],
    }

    cursor: str | None = None
    for _ in range(50):  # hard page cap; 50 * 50 = 2500 open PRs
        args = [
            "api",
            "graphql",
            "-H",
            "Accept: application/vnd.github.merge-info-preview+json",
            "-f",
            f"query={_PR_QUERY}",
            "-f",
            f"owner={owner}",
            "-f",
            f"name={name}",
        ]
        if cursor:
            args += ["-f", f"cursor={cursor}"]
        try:
            data = json.loads(_gh(args))
        except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            snapshot["errors"].append({"stage": "graphql", "error": str(exc)[:200]})
            break

        prs = (
            data.get("data", {})
            .get("repository", {})
            .get("pullRequests", {})
        )
        for node in prs.get("nodes", []) or []:
            commits = (node.get("commits") or {}).get("nodes") or []
            commit = commits[0].get("commit") if commits else {}
            snapshot["open_prs"].append(
                {
                    "number": node.get("number"),
                    "title": node.get("title", ""),
                    "url": node.get("url", ""),
                    "is_draft": bool(node.get("isDraft")),
                    "merge_state_status": node.get("mergeStateStatus"),
                    "auto_merge_enabled": node.get("autoMergeRequest") is not None,
                    "checks": _checks_from_rollup(commit or {}),
                }
            )

        page = prs.get("pageInfo") or {}
        if page.get("hasNextPage"):
            cursor = page.get("endCursor")
        else:
            break

    return snapshot


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def render_body(
    findings: list[dict[str, Any]],
    threshold_minutes: float,
    now: dt.datetime,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    lines: list[str] = []
    lines.append(
        "_Auto-generated by `.github/workflows/merge-stall-check.yml` on "
        f"{now.strftime('%Y-%m-%d %H:%M UTC')}._"
    )
    lines.append("")
    lines.append(
        f"**{len(alarms)} PR(s) have been merge-ready for more than "
        f"{threshold_minutes:g} minutes across two consecutive sweeps, and are "
        "still unmerged.** Every required check is green, GitHub's merge verdict "
        "is CLEAN or BEHIND, and auto-merge is enabled — the only thing left to "
        "act is whatever presses the merge button. Nothing is queued waiting on a "
        "runner (that is a different watchdog). This is the signature of a wedged "
        "auto-merger: green in, nothing out."
    )
    lines.append("")
    lines.append("### Merge-ready but not merging")
    lines.append("")
    for f in alarms:
        lines.append(
            f"- **[#{f['number']}]({f['url']})** — {f['merge_state_status']}, "
            f"merge-ready {f['ready_minutes']:g} min (green since {f['green_since']})"
        )
        title = f.get("title")
        if title:
            lines.append(f"  - {title}")
    lines.append("")
    lines.append("### Where to look")
    lines.append("")
    lines.append(
        "Required checks green + nothing queued + nothing merging points at the "
        "MERGER, not the runners. Cheapest first:"
    )
    lines.append("")
    lines.append(
        "- Shipyard's per-host queue-tick stuck in reap-only mode "
        "(`shipyard runner watch` / `shipyard rescue` on the host that owns the queue)."
    )
    lines.append("- Auto-merge armed on the PRs but the merge method/branch protection changed under it.")
    lines.append("- A required check was renamed in branch protection and no PR can satisfy the new name.")
    lines.append(
        f"- Required set this sweep: `{', '.join(alarms[0]['required_checks'])}` "
        f"(source: {alarms[0]['required_checks_source']})."
        if alarms
        else ""
    )
    lines.append("")
    lines.append(
        "_This tracker updates in place each sweep and closes automatically "
        "once no PR is stuck merge-ready._"
    )
    return "\n".join(line for line in lines if line is not None)


def render_summary(
    findings: list[dict[str, Any]],
    threshold_minutes: float,
    errors: list[dict[str, Any]] | None = None,
) -> str:
    alarms = [f for f in findings if f["level"] == "alarm"]
    pendings = [f for f in findings if f["level"] == "pending"]
    lines = ["## Merge-stall watchdog", ""]
    if errors:
        lines.append(
            f"> **Degraded sweep** — {len(errors)} collection call(s) failed, so "
            "this snapshot may be incomplete. Next sweep in ~30 min."
        )
        lines.append("")
    if not findings:
        lines.append(
            "No PR is merge-ready-and-stuck. Merges are flowing (or nothing is ready)."
        )
        return "\n".join(lines)
    lines.append(
        f"- **{len(alarms)}** alarm (merge-ready >= {threshold_minutes:g} min, two "
        "consecutive sweeps)"
    )
    lines.append(
        f"- **{len(pendings)}** pending (qualified this sweep only — promoted next "
        "sweep if it persists)"
    )
    lines.append("")
    lines.append("| level | PR | merge state | ready (min) |")
    lines.append("| --- | --- | --- | --- |")
    for f in findings:
        lines.append(
            f"| {f['level']} | #{f['number']} | {f['merge_state_status']} | "
            f"{f['ready_minutes']:g} |"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default="", help="owner/name (live collection)")
    ap.add_argument("--base", default="main", help="base branch for required-check lookup")
    ap.add_argument("--snapshot", default="", help="read a recorded snapshot instead of the API")
    ap.add_argument("--prev-state", default="", help="previous-tick stuck-PR state file")
    ap.add_argument("--threshold-minutes", type=float, default=THRESHOLD_MINUTES)
    ap.add_argument("--findings-out", default="findings.json")
    ap.add_argument("--snapshot-out", default="")
    ap.add_argument("--state-out", default="state.json")
    ap.add_argument("--body-out", default="body.md")
    ap.add_argument("--summary-out", default="")
    args = ap.parse_args(argv)

    now = dt.datetime.now(dt.timezone.utc)
    if args.snapshot:
        with open(args.snapshot, encoding="utf-8") as fh:
            snapshot = json.load(fh)
        if snapshot.get("generated_at"):
            now = parse_ts(snapshot["generated_at"])
    else:
        if not args.repo:
            print("merge_stall_watchdog: --repo required without --snapshot", file=sys.stderr)
            return 2
        snapshot = collect_snapshot(args.repo, args.base, now)

    prev_stuck: list[int] = []
    if args.prev_state and os.path.exists(args.prev_state):
        try:
            with open(args.prev_state, encoding="utf-8") as fh:
                prev_stuck = json.load(fh).get("stuck_prs", [])
        except (json.JSONDecodeError, OSError):
            prev_stuck = []

    findings, stuck_now = analyze(snapshot, prev_stuck, now, args.threshold_minutes)
    alarms = [f for f in findings if f["level"] == "alarm"]

    with open(args.findings_out, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    with open(args.state_out, "w", encoding="utf-8") as fh:
        json.dump(
            {"generated_at": now.isoformat(), "stuck_prs": stuck_now}, fh, indent=2
        )
    if args.snapshot_out:
        with open(args.snapshot_out, "w", encoding="utf-8") as fh:
            json.dump(snapshot, fh, indent=2)
    if alarms:
        with open(args.body_out, "w", encoding="utf-8") as fh:
            fh.write(render_body(findings, args.threshold_minutes, now))

    summary = render_summary(findings, args.threshold_minutes, snapshot.get("errors"))
    print(summary)
    if args.summary_out:
        with open(args.summary_out, "a", encoding="utf-8") as fh:
            fh.write(summary + "\n")

    if snapshot.get("errors"):
        print(
            f"note: {len(snapshot['errors'])} collection call(s) failed.",
            file=sys.stderr,
        )

    # Exit 0 regardless of findings: the workflow decides what to do with them.
    # A watchdog that reddens its own run gets ignored.
    print(f"alarm_count={len(alarms)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
