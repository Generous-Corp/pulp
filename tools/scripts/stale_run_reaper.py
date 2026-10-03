#!/usr/bin/env python3
"""Cancel stuck GitHub Actions runs: hung `in_progress` and orphaned `queued`.

Run by `.github/workflows/stale-run-reaper.yml` every 30 minutes.

Two failure modes clog the queue and nothing else in the repo reaps them:

* hung runs -- `in_progress` far past any healthy duration, squatting a runner
  slot until GitHub's 6 h job timeout;
* orphaned queued runs -- `queued` for days on a runner label or branch that no
  longer exists. A job that never starts never times out.

WHY A PLAIN CANCEL IS NOT ENOUGH
--------------------------------
`POST /actions/runs/{id}/cancel` answers 409 for a run whose jobs were never
assigned, and the previous shell reaper read every non-2xx as "likely already
completed". Measured 2026-10-02: thirteen runs had sat `queued` for up to 45
days while the reaper logged "cancel failed ... continuing" for each of them
every 30 minutes, and one it logged as "cancelled" was still queued. So:

1. a stale run gets a plain cancel;
2. if that is refused, it escalates to `force-cancel` -- but only when the run's
   head is NOT the live head of an open pull request, because force-cancel also
   bypasses `always()` cleanup steps and a live head may still matter;
3. a run whose force-cancel is ALSO refused with "has not been queued yet" is a
   GitHub-side phantom: zero jobs, and cancel, force-cancel and delete all
   refuse it. It holds no runner and no concurrency group. It is reported in
   its own table instead of being counted as cancelled, so the summary stops
   claiming progress it did not make. Only GitHub Support can remove one.

The canonical `Build and Test` workflow is skipped: Shipyard owns a narrower,
receipt-fenced recovery protocol for it, and this age janitor must not become a
competing actuator.

Exit status is 0 unless the scan itself could not run.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable

API = "https://api.github.com"
BUILD_WORKFLOW = (".github/workflows/build.yml", 256999733, "Build and Test")
PHANTOM_MARKER = "has not been queued yet"


class ApiError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.message = message


class Api:
    """Minimal REST client; counts its calls so the summary can report them."""

    def __init__(self, token: str, repo: str):
        self.token = token
        self.repo = repo
        self.calls = 0

    def _request(self, method: str, path: str) -> Any:
        self.calls += 1
        request = urllib.request.Request(
            f"{API}{path}",
            method=method,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "pulp-stale-run-reaper",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            try:
                detail = json.loads(detail).get("message", detail)
            except ValueError:
                pass
            raise ApiError(exc.code, str(detail)) from exc
        return json.loads(body) if body.strip() else {}

    def get(self, path: str) -> Any:
        return self._request("GET", path)

    def post(self, path: str) -> Any:
        return self._request("POST", path)

    def paged(self, path: str, key: str) -> list[dict]:
        items: list[dict] = []
        sep = "&" if "?" in path else "?"
        for page in range(1, 51):
            batch = self.get(f"{path}{sep}per_page=100&page={page}")
            chunk = batch.get(key, []) if isinstance(batch, dict) else batch
            items.extend(chunk)
            if len(chunk) < 100:
                break
        return items


@dataclass
class Outcome:
    run_id: int
    name: str
    branch: str
    status: str
    age_min: int
    result: str  # cancel-requested | force-cancel-requested | phantom | live-head | failed
    detail: str = ""


@dataclass
class Report:
    scanned: int = 0
    outcomes: list[Outcome] = field(default_factory=list)

    def by_result(self, *results: str) -> list[Outcome]:
        return [o for o in self.outcomes if o.result in results]


def _epoch(stamp: str) -> float:
    return dt.datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()


def is_build_workflow(run: dict) -> bool:
    """True for the canonical Build and Test workflow; raises on identity drift."""
    path, workflow_id, name = BUILD_WORKFLOW
    hits = (run.get("path") == path, run.get("workflow_id") == workflow_id,
            run.get("name") == name)
    if any(hits) and not all(hits):
        raise SystemExit(
            f"refusing typed-recovery workflow identity drift for run {run.get('id')}: "
            f"name={run.get('name')!r} id={run.get('workflow_id')} path={run.get('path')!r}")
    return all(hits)


def age_minutes(run: dict, status: str, now: float) -> int | None:
    # An in_progress run is aged from execution start, not creation: creation
    # also counts queue time. A queued run never started, so creation IS its age.
    stamp = run.get("created_at")
    if status == "in_progress" and run.get("run_started_at"):
        stamp = run["run_started_at"]
    try:
        return int((now - _epoch(stamp)) // 60)
    except (TypeError, ValueError, AttributeError):
        return None


def cancel(api: Api, run_id: int, live_head: bool) -> tuple[str, str]:
    """Cancel one run, escalating to force-cancel when that is safe."""
    try:
        api.post(f"/repos/{api.repo}/actions/runs/{run_id}/cancel")
        return "cancel-requested", ""
    except ApiError as exc:
        first = exc
    if first.status != 409:
        return "failed", str(first)
    if live_head:
        return "live-head", f"cancel refused ({first.message}); head is a live PR head"
    try:
        api.post(f"/repos/{api.repo}/actions/runs/{run_id}/force-cancel")
        return "force-cancel-requested", f"cancel refused: {first.message}"
    except ApiError as exc:
        if exc.status == 409 and PHANTOM_MARKER in exc.message:
            return "phantom", exc.message
        return "failed", f"force-cancel {exc}"


def reap(api: Api, *, now: float, self_id: int, in_progress_max: int,
         queued_max: int, dry_run: bool = False,
         log: Callable[[str], None] = print) -> Report:
    report = Report()
    live_heads: set[str] | None = None
    for status, limit in (("in_progress", in_progress_max), ("queued", queued_max)):
        log(f"=== status={status} (cutoff {limit} min) ===")
        runs = api.paged(f"/repos/{api.repo}/actions/runs?status={status}", "workflow_runs")
        for run in runs:
            report.scanned += 1
            run_id = run.get("id")
            if not isinstance(run_id, int) or run_id <= 0:
                log("  skip malformed run identity")
                continue
            if run_id == self_id:
                continue
            if is_build_workflow(run):
                log(f"  skip {run_id} (Build and Test is reserved for typed recovery)")
                continue
            age = age_minutes(run, status, now)
            if age is None:
                log(f"  skip {run_id} (unparseable timestamp)")
                continue
            if age <= limit:
                continue
            if live_heads is None:
                live_heads = {
                    pr.get("head", {}).get("sha", "")
                    for pr in api.paged(f"/repos/{api.repo}/pulls?state=open", "")
                }
            live = run.get("head_sha", "") in live_heads
            name = str(run.get("name", ""))
            branch = str(run.get("head_branch") or "(none)")
            if dry_run:
                result, detail = "dry-run", ""
            else:
                result, detail = cancel(api, run_id, live)
            log(f"  STUCK {run_id} '{name}' branch '{branch}' ({age} min {status}) -> {result}"
                + (f" ({detail})" if detail else ""))
            report.outcomes.append(Outcome(run_id, name, branch, status, age, result, detail))
    return report


def summary(report: Report, *, in_progress_max: int, queued_max: int, calls: int) -> str:
    acted = report.by_result("cancel-requested", "force-cancel-requested")
    lines = [
        "## Stale run reaper",
        "",
        f"- Thresholds: `in_progress` > **{in_progress_max} min**, `queued` > **{queued_max} min**",
        f"- Runs scanned: **{report.scanned}**",
        f"- Cancels requested: **{len(acted)}**",
        f"- API calls: {calls}",
        "",
    ]

    def table(title: str, rows: list[Outcome], note: str = "") -> None:
        if not rows:
            return
        lines.extend([f"### {title}", ""])
        if note:
            lines.extend([note, ""])
        lines.append("| Run ID | Workflow | Branch | Status | Age | Result |")
        lines.append("|---|---|---|---|---|---|")
        for o in rows:
            lines.append(f"| {o.run_id} | {o.name} | `{o.branch}` | {o.status} | "
                         f"{o.age_min} min | {o.result} |")
        lines.append("")

    table("Cancelled", acted,
          "_If the same workflow is reaped repeatedly, it has a hang bug worth investigating._")
    table("GitHub-side phantoms (uncancellable)", report.by_result("phantom"),
          "GitHub refuses cancel AND force-cancel with `has not been queued yet`; "
          "these runs have no jobs and hold no runner or concurrency group. "
          "Only GitHub Support can remove them.")
    table("Left alone: head is a live pull-request head", report.by_result("live-head"))
    table("Cancel failed", report.by_result("failed"))
    if not report.outcomes:
        lines.append("No stuck runs detected — CI queue is clean.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("REPO") or os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--self-run-id", type=int, default=int(os.environ.get("SELF_RUN_ID", "0") or 0))
    parser.add_argument("--in-progress-max", type=int,
                        default=int(os.environ.get("IN_PROGRESS_MAX_MINUTES", "240")))
    parser.add_argument("--queued-max", type=int,
                        default=int(os.environ.get("QUEUED_MAX_MINUTES", "480")))
    parser.add_argument("--summary", help="append the markdown summary to this file")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not args.repo or not token:
        print("stale-run-reaper: --repo and GH_TOKEN are required", file=sys.stderr)
        return 2
    api = Api(token, args.repo)
    report = reap(api, now=dt.datetime.now(dt.timezone.utc).timestamp(),
                  self_id=args.self_run_id, in_progress_max=args.in_progress_max,
                  queued_max=args.queued_max, dry_run=args.dry_run)
    text = summary(report, in_progress_max=args.in_progress_max,
                   queued_max=args.queued_max, calls=api.calls)
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as handle:
            handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
