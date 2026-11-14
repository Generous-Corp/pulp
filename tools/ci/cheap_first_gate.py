#!/usr/bin/env python3
"""Hold the native build matrix until the cheap required checks pass at the head.

build.yml's `cheap-first` job runs this. The required `macos` gate costs a
gate VM for about 40 minutes; `drift-fast` and `Enforce version & skill sync`
are required checks that run on hosted runners in a few minutes and live in
their own workflows, so `needs:` cannot reach them. This script reads their
check runs on the head the build tests and decides:

  pass     every cheap check concluded green (or was skipped, neutral,
           cancelled or never appeared): start the native matrix.
  block    a cheap check concluded red at this head: the native matrix does
           not start, and the `macos` alias reports why. The head cannot merge
           anyway, because the red check is itself required.
  unknown  the deadline passed, or the API could not be read: start the
           native matrix. The gate only ever withholds a VM from a head that
           is already red; it never holds one on a doubt.

The `block` verdict is the only one that exits non-zero.

The head is the commit the cheap checks report on: the pull request's head sha
for pull_request, the dispatch commit for a `pr_number` dispatch, and the merge
group's own commit for merge_group. Push runs and plain dispatches have no
cheap checks to wait on and pass at once, and so does a pull request whose base
is not `main` (the version & skill workflow does not run there).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Callable

# The required contexts this gate waits on, by check-run name. Both are
# required on `main` (.github/rulesets/main-protection.json); the selftest
# fails if either stops being required there.
CHEAP_CHECKS = ("drift-fast", "Enforce version & skill sync")

# Check runs are read only from GitHub Actions. Another app able to write
# checks could otherwise post a red run under one of these names and keep the
# native gate from starting.
ACTIONS_APP_SLUG = "github-actions"

RED = frozenset({"failure", "timed_out", "action_required", "startup_failure"})
GREEN = frozenset({"success", "neutral", "skipped"})
# A cancelled or stale run is not evidence either way; it does not hold the VM.
VOID = frozenset({"cancelled", "stale"})

DEFAULT_DEADLINE_SECONDS = 30 * 60
DEFAULT_ABSENT_GRACE_SECONDS = 10 * 60
DEFAULT_INTERVAL_SECONDS = 30


@dataclass(frozen=True)
class CheckState:
    """The newest GitHub Actions check run of one name on the head."""

    name: str
    state: str  # absent | pending | red | green | void
    conclusion: str = ""
    url: str = ""


def classify_run(name: str, runs: list[dict]) -> CheckState:
    """Reduce the check runs of one name to the state of the newest one."""
    own = [
        r for r in runs
        if r.get("name") == name and (r.get("app") or {}).get("slug") == ACTIONS_APP_SLUG
    ]
    if not own:
        return CheckState(name, "absent")
    # A queued rerun has no started_at yet.  created_at is present as soon as
    # GitHub creates the run, so order by it first and use the monotonic run id
    # as the tie breaker.
    newest = max(own, key=lambda r: (r.get("created_at") or r.get("started_at") or "",
                                     r.get("id") or 0))
    url = newest.get("html_url") or ""
    if newest.get("status") != "completed":
        return CheckState(name, "pending", url=url)
    conclusion = newest.get("conclusion") or ""
    if conclusion in RED:
        return CheckState(name, "red", conclusion, url)
    if conclusion in GREEN:
        return CheckState(name, "green", conclusion, url)
    if conclusion in VOID:
        return CheckState(name, "void", conclusion, url)
    # An unknown conclusion is not a red one; do not withhold the VM for it.
    return CheckState(name, "void", conclusion or "unknown", url)


def decide(states: list[CheckState], *, elapsed: float, absent_grace: float) -> str:
    """Return block, pass or wait for one poll's states."""
    if any(s.state == "red" for s in states):
        return "block"
    for s in states:
        if s.state == "pending":
            return "wait"
        if s.state == "absent" and elapsed < absent_grace:
            return "wait"
    return "pass"


def applies(event: str, base_ref: str, dispatch_pr: str) -> tuple[bool, str]:
    """Whether this event has cheap checks on its head to wait for."""
    if event == "merge_group":
        return True, "merge group"
    if event == "pull_request":
        if base_ref != "main":
            return False, f"pull request into {base_ref or 'an unknown base'}, not main"
        return True, "pull request into main"
    if event == "workflow_dispatch" and dispatch_pr:
        return True, f"dispatch of pull request {dispatch_pr}"
    return False, f"{event or 'unknown'} event has no cheap required checks on its head"


Fetch = Callable[[str], list[dict]]


def github_fetch(repo: str, sha: str, token: str, api: str) -> Fetch:
    def fetch(name: str) -> list[dict]:
        query = urllib.parse.urlencode({"check_name": name, "filter": "latest", "per_page": 100})
        req = urllib.request.Request(
            f"{api}/repos/{repo}/commits/{sha}/check-runs?{query}",
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {token}",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.load(resp).get("check_runs", [])

    return fetch


def run_gate(
    fetch: Fetch,
    *,
    deadline: float,
    absent_grace: float,
    interval: float,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    log: Callable[[str], None] = print,
) -> tuple[str, list[CheckState]]:
    """Poll until a decision; return (verdict, final states).

    Every poll refreshes both check names. A terminal run can be superseded by
    a rerun on the same head, and that newer result must be observed before the
    native matrix starts.
    """
    start = clock()
    states: list[CheckState] = []
    while True:
        elapsed = clock() - start
        states = []
        read_failed = False
        for name in CHEAP_CHECKS:
            try:
                state = classify_run(name, fetch(name))
            except (urllib.error.URLError, OSError, ValueError) as exc:
                log(f"cheap-first: could not read {name!r}: {exc}")
                read_failed = True
                state = CheckState(name, "pending")
            states.append(state)
        verdict = decide(states, elapsed=elapsed, absent_grace=absent_grace)
        if verdict == "pass" and read_failed:
            verdict = "wait"
        summary = ", ".join(f"{s.name}={s.conclusion or s.state}" for s in states)
        log(f"cheap-first: t+{int(elapsed)}s {summary} -> {verdict}")
        if verdict in ("block", "pass"):
            return verdict, states
        if elapsed + interval > deadline:
            return "unknown", states
        sleep(interval)


def _write_output(verdict: str, reason: str) -> None:
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(f"verdict={verdict}\n")
            fh.write(f"reason={reason}\n")


def _write_summary(lines: list[str]) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--repo", required=True)
    p.add_argument("--sha", default="")
    p.add_argument("--event", required=True)
    p.add_argument("--base-ref", default="")
    p.add_argument("--dispatch-pr", default="")
    p.add_argument("--mode", default="", help="PULP_CHEAP_FIRST_MODE: enforce (default) or off")
    p.add_argument("--deadline", type=float, default=DEFAULT_DEADLINE_SECONDS)
    p.add_argument("--absent-grace", type=float, default=DEFAULT_ABSENT_GRACE_SECONDS)
    p.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_SECONDS)
    args = p.parse_args(argv)

    mode = (args.mode or "enforce").strip().lower()
    if mode == "off":
        reason = "PULP_CHEAP_FIRST_MODE=off"
        print(f"cheap-first: {reason}; the native matrix starts at once")
        _write_output("pass", reason)
        return 0
    if mode != "enforce":
        print(f"::warning title=cheap-first::unknown PULP_CHEAP_FIRST_MODE {mode!r}; enforcing")

    applicable, why = applies(args.event, args.base_ref, args.dispatch_pr)
    if not applicable:
        print(f"cheap-first: {why}; the native matrix starts at once")
        _write_output("pass", why)
        return 0
    if len(args.sha) != 40:
        reason = f"no head sha to read ({args.sha!r})"
        print(f"::warning title=cheap-first::{reason}; starting the native matrix")
        _write_output("unknown", reason)
        return 0
    token = os.environ.get("GITHUB_TOKEN", "")
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    fetch = github_fetch(args.repo, args.sha, token, api)

    verdict, states = run_gate(
        fetch, deadline=args.deadline, absent_grace=args.absent_grace, interval=args.interval)
    table = ["| check | result |", "|---|---|"] + [
        f"| {s.name} | {s.conclusion or s.state} |" for s in states]
    if verdict == "block":
        red = [s for s in states if s.state == "red"]
        reason = "; ".join(f"{s.name} concluded {s.conclusion} at {args.sha[:12]}" for s in red)
        for s in red:
            print(f"::error title=cheap-first: {s.name} is red::{s.name} concluded "
                  f"{s.conclusion} at {args.sha}; the native macOS matrix is not started. "
                  f"Fix it and push, or re-run its check and then this run's failed jobs. {s.url}")
        _write_summary(["### Native matrix not started: a cheap required check is red", "",
                        *table])
        _write_output("block", reason)
        return 1
    if verdict == "unknown":
        reason = "the cheap checks did not all report in time; starting the native matrix"
        print(f"::warning title=cheap-first::{reason}")
    else:
        reason = "the cheap required checks are green at this head"
    _write_summary([f"### cheap-first: {verdict}", "", *table])
    _write_output(verdict, reason)
    return 0


if __name__ == "__main__":
    sys.exit(main())
