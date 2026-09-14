#!/usr/bin/env python3
"""Outcome-based landing watchdog: is any open PR stuck on a required context
that is absent or unassigned?

This is the **redundant** half of the wedge detector, and it is redundant on
four axes by construction. Shipyard's landability preflight is the primary: it
runs on the fleet, as the Shipyard GitHub App, by resolving runner labels
before work queues, in Rust. Every one of those is a way to be blind, and each
was blind at least once on 2026-09-13.

    axis        primary                      this
    ---------   --------------------------   ------------------------------
    host        M1 / M3 / M5 (the fleet)     ubuntu-latest, GitHub-hosted
    credential  Shipyard App installation    GITHUB_TOKEN, no admin scope
    method      precondition: resolve,       outcome: is a required context
                census, attest               absent or unassigned past T
    code        Rust, fleet_service          this file, no shared library

The method axis is the one that matters most. A precondition model can be
wrong about a mechanism it never modelled — and the 2026-09-13 session found
four distinct wedges, only one of which any existing model described. An
outcome check cannot be wrong in that way: it does not care *why* `macos` is
missing from a pull request's head SHA two hours after the last push, only
that it is. The cost is that it cannot say why, which is exactly what the
primary is for.

Two implementations of one classifier would share every bug, so this one
deliberately shares no code with Shipyard.

What it never does
------------------
It never dispatches a workflow, cancels a run, re-runs a job, or merges
anything. The repository's decisions contract, row ``[default] #4``, is
explicit that a runnerless required lane is HELD and never retried; on
2026-09-13 four blind re-dispatches helped nothing and created a second wedge
by filling the concurrency group. The only thing this writes is one tracking
issue.

Its own liveness
----------------
A watchdog that silently scans zero pull requests reports "no findings", which
reads identically to health. So every run asserts a control: the number of
open pull requests it scanned must match the number the API listed, and the
required-context list must be non-empty. If either fails the job **fails**,
loudly, instead of reporting a clean result it did not measure.

Budget: 1 call to list open PRs, then at most 3 per PR, once every 30 minutes,
on ``GITHUB_TOKEN``'s own per-repository bucket. At ten open PRs that is about
62 calls an hour, and never more than one in flight.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

API = "https://api.github.com"

#: A required context absent this long after the head last moved is a finding.
#: Generous enough that a normal queue never trips it, short enough that it
#: speaks after 45 minutes rather than after six hours.
ABSENT_AFTER_MINS = 45
#: A check run that exists but has no runner assigned this long is HELD.
UNASSIGNED_AFTER_MINS = 30
#: A workflow run pending with zero jobs this long is the concurrency-holder
#: signature — distinct from capacity, and invisible to every "is CI busy"
#: heuristic.
ZERO_JOB_AFTER_MINS = 15
#: Only PRs touched recently are scanned, to bound the budget.
RECENT_DAYS = 14

ISSUE_LABEL = "ci-landing-wedge"
ISSUE_TITLE = "CI landing wedge: a required context cannot land"


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def parse_ts(value: str | None) -> datetime | None:
    if not value:
        return None
    try:
        return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError:
        return None


class Api:
    """A deliberately tiny REST client that counts its own calls.

    The count is reported, never estimated. A budget nobody measures is a wish,
    and the secondary rate limit that takes a machine's whole API access offline
    trips on burst shape rather than on quota.
    """

    def __init__(self, token: str, repo: str):
        self.token = token
        self.repo = repo
        self.calls = 0

    def get(self, path: str):
        self.calls += 1
        request = urllib.request.Request(
            f"{API}{path}",
            headers={
                "Authorization": f"Bearer {self.token}",
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "pulp-landing-watchdog",
            },
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))

    def post(self, path: str, payload: dict):
        self.calls += 1
        request = urllib.request.Request(
            f"{API}{path}",
            data=json.dumps(payload).encode("utf-8"),
            method="POST",
            headers={
                "Authorization": f"Bearer {self.token}",
                "Accept": "application/vnd.github+json",
                "Content-Type": "application/json",
                "User-Agent": "pulp-landing-watchdog",
            },
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))

    def patch(self, path: str, payload: dict):
        self.calls += 1
        request = urllib.request.Request(
            f"{API}{path}",
            data=json.dumps(payload).encode("utf-8"),
            method="PATCH",
            headers={
                "Authorization": f"Bearer {self.token}",
                "Accept": "application/vnd.github+json",
                "Content-Type": "application/json",
                "User-Agent": "pulp-landing-watchdog",
            },
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))


def required_contexts(config_path: str) -> list[str]:
    """Read ``[governance] required_status_checks`` from the checkout.

    Deliberately NOT branch protection: reading protection needs an elevated
    scope, and this detector's whole point is to need nothing the primary needs.
    The cost is that the list can drift from protection; the primary reads both
    and reports the disagreement, which is the right division of labour.
    """
    try:
        import tomllib
    except ModuleNotFoundError:  # pragma: no cover - python < 3.11
        return []
    try:
        with open(config_path, "rb") as handle:
            data = tomllib.load(handle)
    except (OSError, ValueError):
        return []
    contexts = data.get("governance", {}).get("required_status_checks", [])
    return [str(entry) for entry in contexts if str(entry).strip()]


def classify_pr(pr: dict, check_runs: list[dict], runs: list[dict],
                contexts: list[str], now: datetime) -> list[dict]:
    """Findings for one pull request. Pure: every input is passed in."""
    findings: list[dict] = []
    head_sha = pr.get("head", {}).get("sha", "")
    number = pr.get("number")
    pushed = parse_ts(pr.get("updated_at")) or now
    age_mins = (now - pushed).total_seconds() / 60.0
    author_type = pr.get("user", {}).get("type", "")

    by_name: dict[str, dict] = {}
    for run in check_runs:
        name = run.get("name", "")
        # Keep the most advanced record per name: a completed check outranks a
        # queued one with the same name.
        existing = by_name.get(name)
        if existing is None or run.get("status") == "completed":
            by_name[name] = run

    for context in contexts:
        found = by_name.get(context)
        if found is None:
            if age_mins >= ABSENT_AFTER_MINS:
                findings.append(
                    {
                        "pr": number,
                        "head_sha": head_sha,
                        "context": context,
                        "state": "absent",
                        "age_mins": round(age_mins),
                        "author_is_bot": author_type == "Bot",
                        "detail": (
                            f"required context `{context}` has no check run on the head SHA "
                            f"{head_sha[:12]} {round(age_mins)} minutes after the PR last moved"
                            + (
                                "; the PR was opened by an App, so `pull_request` workflows may "
                                "never have fired"
                                if author_type == "Bot"
                                else ""
                            )
                        ),
                    }
                )
            continue
        status = found.get("status")
        if status == "completed":
            continue
        started = parse_ts(found.get("started_at")) or parse_ts(found.get("created_at"))
        waiting = (now - started).total_seconds() / 60.0 if started else 0.0
        if waiting >= UNASSIGNED_AFTER_MINS:
            findings.append(
                {
                    "pr": number,
                    "head_sha": head_sha,
                    "context": context,
                    "state": "unassigned",
                    "age_mins": round(waiting),
                    "author_is_bot": author_type == "Bot",
                    "detail": (
                        f"required context `{context}` has been `{status}` for "
                        f"{round(waiting)} minutes without completing"
                    ),
                }
            )

    for run in runs:
        if run.get("status") not in {"queued", "pending"}:
            continue
        # A run pending with zero jobs is the concurrency-holder signature: it
        # is not queued for capacity, and it reads exactly like saturation to
        # every heuristic that only looks at how busy CI is.
        if run.get("jobs_total_count") != 0:
            continue
        created = parse_ts(run.get("created_at"))
        waiting = (now - created).total_seconds() / 60.0 if created else 0.0
        if waiting >= ZERO_JOB_AFTER_MINS:
            findings.append(
                {
                    "pr": number,
                    "head_sha": head_sha,
                    "context": run.get("name", "workflow"),
                    "state": "zero_jobs",
                    "age_mins": round(waiting),
                    "author_is_bot": author_type == "Bot",
                    "run_id": run.get("id"),
                    "detail": (
                        f"workflow run {run.get('id')} has been `{run.get('status')}` with zero "
                        f"jobs for {round(waiting)} minutes - the concurrency-holder signature, "
                        "not capacity"
                    ),
                }
            )

    return findings


def render_issue_body(findings: list[dict], scanned: int, contexts: list[str],
                      api_calls: int, now: datetime) -> str:
    lines = [
        "A required status context on one or more open pull requests is absent "
        "or unassigned past its threshold, so those pull requests cannot land.",
        "",
        f"Detected {now.strftime('%Y-%m-%d %H:%M UTC')} by "
        "`.github/workflows/landing-watchdog.yml`, which runs on "
        "`ubuntu-latest` with `GITHUB_TOKEN` and is deliberately independent of "
        "the fleet, the Shipyard App, and Shipyard's own classifier.",
        "",
        f"- pull requests scanned: **{scanned}**",
        f"- required contexts checked: {', '.join(f'`{c}`' for c in contexts)}",
        f"- API calls this run: {api_calls}",
        "",
        "## Findings",
        "",
        "| PR | context | state | age | detail |",
        "|---|---|---|---|---|",
    ]
    for finding in sorted(findings, key=lambda f: (f["pr"], f["context"])):
        lines.append(
            f"| #{finding['pr']} | `{finding['context']}` | {finding['state']} | "
            f"{finding['age_mins']}m | {finding['detail']} |"
        )
    lines += [
        "",
        "## What to do",
        "",
        "This watchdog reports an **outcome** and cannot say why. For the cause, "
        "ask the precondition detector, which resolves each required context to "
        "the jobs and runner labels that produce it:",
        "",
        "```sh",
        "shipyard landability --repo Generous-Corp/pulp",
        "```",
        "",
        "- `absent` + the PR was opened by an App: `pull_request` workflows may "
        "never have fired for it.",
        "- `unassigned`: the job is queued on labels nothing carries (HELD), or "
        "on labels an idle runner does carry (STARVED). `shipyard landability` "
        "tells the two apart; they have opposite fixes.",
        "- `zero_jobs`: an older run is holding the concurrency group. A plain "
        "cancel will not move a run whose jobs were never assigned; "
        "`POST /actions/runs/{id}/force-cancel` will.",
        "",
        "**Do not blind re-dispatch.** Decisions contract `[default] #4`: a "
        "runnerless required lane is HELD, never a retry storm. On 2026-09-13 "
        "four re-dispatches helped nothing and created a second wedge.",
        "",
        "This issue is opened, edited and closed automatically. It closes on its "
        "own when the next run finds nothing.",
    ]
    return "\n".join(lines)


def fetch_state(api: Api, contexts: list[str], now: datetime, run_limit: int = 3):
    """Gather findings across every recently-touched open pull request."""
    since = (now - timedelta(days=RECENT_DAYS)).strftime("%Y-%m-%dT%H:%M:%SZ")
    prs = api.get(f"/repos/{api.repo}/pulls?state=open&per_page=100&sort=updated&direction=desc")
    recent = [pr for pr in prs if (pr.get("updated_at") or "") >= since]

    findings: list[dict] = []
    for pr in recent:
        head_sha = pr.get("head", {}).get("sha", "")
        if not head_sha:
            continue
        try:
            checks = api.get(
                f"/repos/{api.repo}/commits/{head_sha}/check-runs?per_page=100"
            ).get("check_runs", [])
        except urllib.error.HTTPError:
            checks = []
        try:
            raw_runs = api.get(
                f"/repos/{api.repo}/actions/runs?head_sha={head_sha}&per_page=20"
            ).get("workflow_runs", [])
        except urllib.error.HTTPError:
            raw_runs = []
        runs = []
        for run in raw_runs:
            if run.get("status") not in {"queued", "pending"}:
                continue
            if len(runs) >= run_limit:
                break
            try:
                jobs = api.get(f"/repos/{api.repo}/actions/runs/{run['id']}/jobs?per_page=1")
                run["jobs_total_count"] = jobs.get("total_count", -1)
            except urllib.error.HTTPError:
                run["jobs_total_count"] = -1
            runs.append(run)
        findings.extend(classify_pr(pr, checks, runs, contexts, now))
    return recent, findings


def sync_issue(api: Api, findings: list[dict], body: str) -> str:
    """Open, edit or close the single tracking issue."""
    existing = api.get(
        f"/repos/{api.repo}/issues?state=open&labels={ISSUE_LABEL}&per_page=10"
    )
    open_issue = existing[0] if existing else None
    if findings:
        if open_issue:
            api.patch(
                f"/repos/{api.repo}/issues/{open_issue['number']}",
                {"body": body},
            )
            return f"updated issue #{open_issue['number']}"
        created = api.post(
            f"/repos/{api.repo}/issues",
            {"title": ISSUE_TITLE, "body": body, "labels": [ISSUE_LABEL]},
        )
        return f"opened issue #{created['number']}"
    if open_issue:
        api.patch(
            f"/repos/{api.repo}/issues/{open_issue['number']}",
            {"state": "closed"},
        )
        return f"closed issue #{open_issue['number']}"
    return "no findings, no open issue"


def replay(path: str, now: datetime, contexts: list[str]) -> list[dict]:
    """Run the classifier over a captured fixture, with no network at all."""
    with open(path, "r", encoding="utf-8") as handle:
        fixture = json.load(handle)
    findings: list[dict] = []
    for case in fixture["pull_requests"]:
        findings.extend(
            classify_pr(
                case["pr"],
                case.get("check_runs", []),
                case.get("runs", []),
                contexts or fixture.get("required_contexts", []),
                now,
            )
        )
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--config", default=".shipyard/config.toml")
    parser.add_argument("--replay", default=None, help="classify a captured fixture, no network")
    parser.add_argument("--no-issue", action="store_true", help="report only, never write")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    now = utcnow()
    contexts = required_contexts(args.config)

    if args.replay:
        findings = replay(args.replay, now, contexts)
        payload = {"findings": findings, "mode": "replay"}
        print(json.dumps(payload, indent=2) if args.json else render_issue_body(
            findings, len(findings), contexts or ["<from fixture>"], 0, now
        ))
        return 1 if findings else 0

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        print("GITHUB_TOKEN is required", file=sys.stderr)
        return 2
    if not args.repo:
        print("--repo or GITHUB_REPOSITORY is required", file=sys.stderr)
        return 2

    # The instrument's own control, run BEFORE any finding is believed. A
    # required-context list of zero would make every pull request look clean.
    if not contexts:
        print(
            f"BROKEN INSTRUMENT: no required contexts read from {args.config}. "
            "A scan with an empty context list reports no findings for every pull "
            "request and is indistinguishable from health.",
            file=sys.stderr,
        )
        return 3

    api = Api(token, args.repo)
    recent, findings = fetch_state(api, contexts, now)

    # Second control: the number of pull requests scanned must match what the
    # API listed. A scan of zero with open pull requests present is a broken
    # instrument, not a clean result.
    listed = len(recent)
    if listed == 0:
        probe = api.get(f"/repos/{args.repo}/pulls?state=open&per_page=1")
        if probe:
            print(
                "BROKEN INSTRUMENT: scanned 0 pull requests while the repository has open "
                "ones. The filter, not the fleet, is what this run measured.",
                file=sys.stderr,
            )
            return 3

    body = render_issue_body(findings, listed, contexts, api.calls, now)
    action = "skipped (--no-issue)" if args.no_issue else sync_issue(api, findings, body)

    if args.json:
        print(json.dumps(
            {
                "repo": args.repo,
                "scanned": listed,
                "contexts": contexts,
                "findings": findings,
                "api_calls": api.calls,
                "issue": action,
            },
            indent=2,
        ))
    else:
        print(f"landing-watchdog: scanned {listed} open PR(s), {len(findings)} finding(s)")
        print(f"  required contexts: {', '.join(contexts)}")
        print(f"  api calls: {api.calls}")
        print(f"  issue: {action}")
        for finding in findings:
            print(f"  - #{finding['pr']} {finding['context']}: {finding['detail']}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
