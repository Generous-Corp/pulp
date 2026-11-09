#!/usr/bin/env python3
"""Run an approved head's deferred affected tests before the merge queue does.

A pull-request head's `macos` run spends at most ten minutes on the ctests its
diff reaches (tools/ci/pr_head_affected_tests.py). Tests the budget pushes out
are deferred to the merge queue, so for a large diff the merge group is the
first place they run, and a failure there ejects the whole batch. In the week
of 2026-11-02, 9 of 10 "passes on the head, fails merged" ejections failed on
the head alone, in tests that run had deferred or excluded.

This closes that gap without a new required check and without a new writer
of merge state. Once a head is approved, its `macos` job runs again on the
same head, and that attempt runs exactly the tests the budgeted attempt
deferred. Its result is the head's `macos` check, which branch protection
already requires, so the steward cannot arm the head while it runs and will
not arm it if a deferred test fails.

Subcommands:

  probe   build.yml, before the affected-test step: exit 0 when this run is an
          admission run (the head's `review-verdict` check is green and the
          mode is `enforce`), 1 otherwise. Reads only; never fails the job.
  prior   build.yml, admission runs on attempt 2 or later: write the newest
          earlier attempt's affected-test summary (the artifact
          `pr-head-affected-tests-a<attempt>`) to --out; exit 1 when there is
          none, and the whole selection runs.
  decide  deferred-tests-admission.yml: from the triggering event, decide
          whether the head's `macos` job must run again, and in `enforce`
          mode re-run it. Emits one `deferred-tests-admission` notice.

The re-run is bounded: only a completed, successful, budgeted attempt that
deferred tests qualifies; the admission attempt reports `admission: true`, so
it is never re-run by this workflow again, and nothing is re-run at or after
attempt MAX_ATTEMPT. A red `macos` is a terminal verdict and is never re-run.
A head the merge queue holds, or that is armed, is never touched. Every read
failure decides "no re-run", which is today's behaviour: the merge group
still runs the full suite.

The summary the decision reads is written by the pull request's own build.yml,
so the code under test could suppress it. That only withholds the extra run;
it cannot admit anything, because admission still needs a green `macos` and
the merge group still runs the full suite.

Mode comes from the repository variable PULP_DEFERRED_ADMISSION: `enforce`
re-runs, anything else (the default) is `shadow`, which logs what it would do.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

API = "https://api.github.com"
FLEET_APP = "shipyard-local[bot]"
VERDICT_CHECK = "review-verdict"
GATE_JOB = "macos"
BUILD_WORKFLOW = "build.yml"
ARTIFACT_PREFIX = "pr-head-affected-tests-a"
SUMMARY_FILE = "pr-affected-summary.json"
SUMMARY_SCHEMA = "pulp-pr-head-affected-tests/v1"
NOTICE_TITLE = "deferred-tests-admission"
NOTICE_SCHEMA = "pulp-deferred-tests-admission/v1"
# A run is never re-run by this workflow at or past this attempt.
MAX_ATTEMPT = 3
ENFORCE = "enforce"

Fetch = Callable[[str, str, Any], Any]


class ApiError(Exception):
    """A GitHub read or write that did not succeed."""


def mode_from(value: str | None) -> str:
    return ENFORCE if (value or "").strip().lower() == ENFORCE else "shadow"


def http(method: str, path: str, body: Any = None, *, raw: bool = False) -> Any:
    """One GitHub API call with the job's token; JSON, or bytes when ``raw``."""
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
    url = path if path.startswith("https://") else f"{API}/{path.lstrip('/')}"
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("X-GitHub-Api-Version", "2022-11-28")
    if token:
        # Not forwarded on a redirect: an artifact download redirects to
        # signed storage, which refuses a second credential.
        request.add_unredirected_header("Authorization", f"Bearer {token}")
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = response.read()
    except (urllib.error.URLError, OSError) as exc:
        raise ApiError(f"{method} {path}: {exc}") from exc
    if raw:
        return payload
    return json.loads(payload) if payload.strip() else {}


def latest_verdict_check(fetch: Fetch, repo: str, head: str) -> dict | None:
    """The newest `review-verdict` check run GitHub Actions left on ``head``."""
    runs = fetch("GET", f"repos/{repo}/commits/{head}/check-runs?check_name={VERDICT_CHECK}"
                 "&filter=latest&per_page=100", None).get("check_runs", [])
    runs = [r for r in runs if (r.get("app") or {}).get("slug") == "github-actions"]
    return max(runs, key=lambda r: r.get("id", 0)) if runs else None


def head_approved(fetch: Fetch, repo: str, head: str) -> tuple[bool, str]:
    """Whether ``head``'s required `review-verdict` check is green."""
    check = latest_verdict_check(fetch, repo, head)
    if check is None:
        return False, f"no {VERDICT_CHECK} check run on {head[:12]}"
    if check.get("status") != "completed":
        return False, f"{VERDICT_CHECK} on {head[:12]} is {check.get('status')}"
    if check.get("conclusion") != "success":
        return False, f"{VERDICT_CHECK} on {head[:12]} concluded {check.get('conclusion')}"
    return True, f"{VERDICT_CHECK} on {head[:12]} is green"


def comment_approves(comment: dict, head: str) -> bool:
    """A fleet-App verdict whose last line approves exactly ``head``.

    The signer and session rules belong to `review-verdict`; this only lets
    the comment's own event start the re-run without waiting for that check.
    The admission attempt itself still reads `review-verdict` before it widens
    its budget.
    """
    if (comment.get("user") or {}).get("login") != FLEET_APP:
        return False
    lines = [line.rstrip() for line in (comment.get("body") or "").split("\n") if line.strip()]
    if not lines or not any(line.startswith("reviewer: ") for line in lines):
        return False
    return lines[-1] == f"verdict: APPROVE head:{head}"


def artifact_summary(fetch: Fetch, repo: str, run_id: int, attempt: int) -> dict | None:
    """The affected-test summary one attempt uploaded, or None."""
    name = f"{ARTIFACT_PREFIX}{attempt}"
    listing = fetch("GET", f"repos/{repo}/actions/runs/{run_id}/artifacts?name={name}"
                    "&per_page=100", None)
    found = [a for a in listing.get("artifacts", [])
             if a.get("name") == name and not a.get("expired")]
    if not found:
        return None
    artifact = max(found, key=lambda a: a.get("id", 0))
    blob = fetch("GET_RAW", f"repos/{repo}/actions/artifacts/{artifact['id']}/zip", None)
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            summary = json.loads(archive.read(SUMMARY_FILE))
    except (KeyError, ValueError, zipfile.BadZipFile) as exc:
        raise ApiError(f"artifact {name} of run {run_id} is unreadable: {exc}") from exc
    return summary if isinstance(summary, dict) else None


def deferred_count(summary: dict) -> int:
    """Tests the attempt left unrun for budget.

    `skipped_for_budget` counts every budget skip; `deferred_to_merge_queue`
    is its ordinary subset (a merge-only skip already fails the step).
    """
    return max(int(summary.get("skipped_for_budget") or 0),
               int(summary.get("deferred_to_merge_queue") or 0))


@dataclass
class Facts:
    """What the decision needs, read from GitHub."""
    pr: int
    open: bool = True
    same_repo: bool = True
    head: str = ""
    event_head: str = ""
    approved: bool = False
    approval: str = ""
    armed_or_queued: bool | None = False
    run_id: int | None = None
    attempt: int = 0
    job_id: int | None = None
    job_status: str = ""
    job_conclusion: str = ""
    summary: dict | None = None
    errors: list[str] = field(default_factory=list)


def decide(facts: Facts) -> tuple[bool, str]:
    """(re-run, why). Every doubt is "no re-run": the queue still runs everything."""
    if facts.errors:
        return False, "could not read: " + "; ".join(facts.errors)
    if not facts.open:
        return False, "the pull request is not open"
    if not facts.same_repo:
        return False, "fork heads are never re-run with repository secrets"
    if facts.event_head and facts.event_head != facts.head:
        return False, f"the event names {facts.event_head[:12]}, not the current head"
    if not facts.approved:
        return False, f"the head is not approved ({facts.approval})"
    if facts.armed_or_queued is None:
        return False, "cannot tell whether the merge queue holds the head"
    if facts.armed_or_queued:
        return False, "the head is armed or in the merge queue; it is not touched"
    if facts.run_id is None or facts.job_id is None:
        return False, f"no pull_request {GATE_JOB} job on the head"
    if facts.job_status != "completed":
        return False, f"{GATE_JOB} is {facts.job_status}; its completion decides"
    if facts.job_conclusion != "success":
        return False, (f"{GATE_JOB} concluded {facts.job_conclusion}: a terminal verdict "
                       "is not re-run")
    if facts.summary is None:
        return False, (f"attempt {facts.attempt} uploaded no {ARTIFACT_PREFIX}{facts.attempt} "
                       "summary (the step did not run, or the head predates it)")
    if facts.summary.get("schema") != SUMMARY_SCHEMA:
        return False, "the summary has an unknown schema"
    if facts.summary.get("admission"):
        return False, f"attempt {facts.attempt} was already the admission run"
    if deferred_count(facts.summary) == 0:
        return False, f"attempt {facts.attempt} deferred nothing"
    if facts.attempt >= MAX_ATTEMPT:
        return False, f"attempt {facts.attempt} is at the re-run limit ({MAX_ATTEMPT})"
    return True, (f"attempt {facts.attempt} deferred {deferred_count(facts.summary)} "
                  "affected test(s) to the merge queue")


def gather(fetch: Fetch, repo: str, event_name: str, event: dict) -> Facts:
    """Read the facts for the pull request the event names."""
    pr_number, event_head, comment = None, "", None
    if event_name == "issue_comment":
        if (event.get("issue") or {}).get("pull_request"):
            pr_number = event["issue"]["number"]
        comment = event.get("comment") or {}
    elif event_name == "workflow_run":
        run = event.get("workflow_run") or {}
        event_head = run.get("head_sha") or ""
        prs = [p.get("number") for p in run.get("pull_requests") or []]
        if not prs and event_head:
            # An App-opened pull request's run lists no pull request; find the
            # open same-repository one whose head is this commit.
            try:
                prs = [p.get("number") for p in fetch("GET", f"repos/{repo}/commits/{event_head}/pulls",
                                                      None) or []
                       if p.get("state") == "open" and (p.get("head") or {}).get("sha") == event_head
                       and ((p.get("head") or {}).get("repo") or {}).get("full_name") == repo]
            except ApiError:
                prs = []
        if len(prs) == 1:
            pr_number = prs[0]
    facts = Facts(pr=int(pr_number or 0), event_head=event_head)
    if not pr_number:
        facts.errors.append("the event names no single pull request")
        return facts
    try:
        pull = fetch("GET", f"repos/{repo}/pulls/{pr_number}", None)
        facts.open = pull.get("state") == "open"
        facts.head = (pull.get("head") or {}).get("sha") or ""
        facts.same_repo = ((pull.get("head") or {}).get("repo") or {}).get("full_name") == repo
        if comment is not None and comment_approves(comment, facts.head):
            facts.approved, facts.approval = True, "the triggering comment approves the head"
        else:
            facts.approved, facts.approval = head_approved(fetch, repo, facts.head)
        facts.armed_or_queued = armed_or_queued(fetch, repo, int(pr_number))
        locate_gate_job(fetch, repo, facts)
    except ApiError as exc:
        facts.errors.append(str(exc))
    return facts


def armed_or_queued(fetch: Fetch, repo: str, pr: int) -> bool | None:
    """True when auto-merge is armed or the queue holds the pull request.

    REST `auto_merge` reads null for a queued pull request, so the queue entry
    is read separately (contract row 11).
    """
    owner, name = repo.split("/", 1)
    query = ("query($o:String!,$r:String!,$n:Int!){repository(owner:$o,name:$r){"
             "pullRequest(number:$n){autoMergeRequest{enabledAt} mergeQueueEntry{state}}}}")
    data = fetch("POST", "graphql", {"query": query, "variables": {"o": owner, "r": name, "n": pr}})
    pull = (((data or {}).get("data") or {}).get("repository") or {}).get("pullRequest")
    if not isinstance(pull, dict):
        return None
    return bool(pull.get("autoMergeRequest")) or bool(pull.get("mergeQueueEntry"))


def locate_gate_job(fetch: Fetch, repo: str, facts: Facts) -> None:
    """The newest pull_request Build and Test run on the head, its gate job and summary."""
    runs = fetch("GET", f"repos/{repo}/actions/workflows/{BUILD_WORKFLOW}/runs?head_sha="
                 f"{facts.head}&event=pull_request&per_page=20", None).get("workflow_runs", [])
    if not runs:
        return
    run = max(runs, key=lambda r: r.get("id", 0))
    facts.run_id = int(run["id"])
    facts.attempt = int(run.get("run_attempt") or 1)
    jobs = fetch("GET", f"repos/{repo}/actions/runs/{facts.run_id}/attempts/{facts.attempt}"
                 "/jobs?per_page=100", None).get("jobs", [])
    gate = [j for j in jobs if j.get("name") == GATE_JOB]
    if len(gate) != 1:
        return
    facts.job_id = int(gate[0]["id"])
    facts.job_status = gate[0].get("status") or ""
    facts.job_conclusion = gate[0].get("conclusion") or ""
    if facts.job_status == "completed":
        facts.summary = artifact_summary(fetch, repo, facts.run_id, facts.attempt)


def default_fetch(method: str, path: str, body: Any) -> Any:
    if method == "GET_RAW":
        return http("GET", path, raw=True)
    return http(method, path, body)


def notice(record: dict) -> str:
    return f"::notice title={NOTICE_TITLE}::{json.dumps(record, sort_keys=True)}"


def cmd_decide(args: argparse.Namespace, fetch: Fetch = default_fetch) -> int:
    event = json.loads(Path(args.event_path).read_text(encoding="utf-8"))
    mode = mode_from(args.mode)
    facts = gather(fetch, args.repo, args.event_name, event)
    rerun, why = decide(facts)
    action = "none"
    if rerun:
        action = "would-rerun" if mode != ENFORCE else "rerun"
        if mode == ENFORCE:
            try:
                fetch("POST", f"repos/{args.repo}/actions/jobs/{facts.job_id}/rerun", {})
            except ApiError as exc:
                action, why = "rerun-failed", f"{why}; the re-run request failed: {exc}"
    record = {"schema": NOTICE_SCHEMA, "mode": mode, "action": action, "reason": why,
              "pr": facts.pr, "head": facts.head, "run_id": facts.run_id,
              "attempt": facts.attempt,
              "deferred": deferred_count(facts.summary) if facts.summary else None}
    print(notice(record))
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as out:
            out.write(f"### deferred tests before admission: {action}\n\n"
                      f"PR {facts.pr} head `{facts.head[:12]}` ({mode}): {why}\n")
    # A failed write request is reported, never retried from here (row 4).
    return 0


def cmd_probe(args: argparse.Namespace, fetch: Fetch = default_fetch) -> int:
    mode = mode_from(args.mode)
    try:
        approved, why = head_approved(fetch, args.repo, args.head)
    except ApiError as exc:
        approved, why = False, f"cannot read {VERDICT_CHECK}: {exc}"
    if approved and mode != ENFORCE:
        print(f"deferred-admission: {why}; shadow mode, so this run keeps the ten-minute budget")
        return 1
    print(f"deferred-admission: {'admission run' if approved else 'budgeted run'} ({why})")
    return 0 if approved else 1


def cmd_prior(args: argparse.Namespace, fetch: Fetch = default_fetch) -> int:
    for attempt in range(args.attempt - 1, 0, -1):
        try:
            summary = artifact_summary(fetch, args.repo, args.run_id, attempt)
        except ApiError as exc:
            print(f"deferred-admission: attempt {attempt}'s summary is unreadable ({exc}); "
                  "running the whole selection")
            return 1
        if summary is not None:
            Path(args.out).write_text(json.dumps(summary), encoding="utf-8")
            print(f"deferred-admission: using attempt {attempt}'s summary")
            return 0
    print("deferred-admission: no earlier attempt uploaded a summary; running the whole selection")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    probe = sub.add_parser("probe")
    probe.add_argument("--repo", required=True)
    probe.add_argument("--head", required=True)
    probe.add_argument("--mode", default=os.environ.get("PULP_DEFERRED_ADMISSION", ""))
    prior = sub.add_parser("prior")
    prior.add_argument("--repo", required=True)
    prior.add_argument("--run-id", type=int, required=True)
    prior.add_argument("--attempt", type=int, required=True)
    prior.add_argument("--out", required=True)
    dec = sub.add_parser("decide")
    dec.add_argument("--repo", required=True)
    dec.add_argument("--event-name", required=True)
    dec.add_argument("--event-path", required=True)
    dec.add_argument("--mode", default=os.environ.get("PULP_DEFERRED_ADMISSION", ""))
    args = parser.parse_args(argv)
    return {"probe": cmd_probe, "prior": cmd_prior, "decide": cmd_decide}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
