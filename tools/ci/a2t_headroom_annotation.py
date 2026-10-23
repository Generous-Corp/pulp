#!/usr/bin/env python3
"""Announce how full the A2T scope-history window is, before its guard fails.

`test_scope_touching_history_keeps_headroom_under_its_limit` (ctest
`gpu-trace-overhead-acceptance-selftest`, on the required `macos` gate) fails
once the scope-touching revision count passes 75% of
`A2T_SCOPE_HISTORY_LIMIT`, and from then on every gate run is red until
`A2T_SCOPE_HISTORY_BASE` is re-pinned (decisions contract row 22). The count
climbs with ordinary traffic, so that failure is a date, not a surprise, but
only if someone sees the climb.

When the test runs with `PULP_A2T_HEADROOM_OUT` set it writes its measurement
there. This reads it and prints one annotation on the gate job:

- a `::warning` from `EARLY_WARNING_RATIO` of the limit, naming the re-pin;
- a `::notice` with the numbers below it, so a missing annotation is itself
  visible as "not measured";
- a `::warning` when the file is missing or unreadable after the suite ran,
  because a silent instrument is the failure this exists to prevent.

It never fails the step: the test is the gate, this is the advance notice.

An annotation on a job nobody opens is easy to miss, and main once reached the
75% guard with the early warning printed and unread. So the push-to-main run
also keeps ONE tracking issue, found by a hidden marker: opened or updated while
the count is at or past the early-warning share (headroom below 20% of the
guard's budget), carrying the count, the limit and the re-pin instruction, and
closed once a re-pin brings it back under. A measurement that could not be read
never opens or closes the issue.

    a2t_headroom_annotation.py <measurement.json>
    a2t_headroom_annotation.py --reconcile-issue --repo <owner/name> <measurement.json>
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Callable

TITLE = "a2t-scope-history-headroom"
EARLY_WARNING_RATIO = 0.60
REPIN = ("re-pin A2T_SCOPE_HISTORY_BASE on protected main, retaining about 25% of the "
         "window; never raise A2T_SCOPE_HISTORY_LIMIT (decisions contract row 22)")


def classify(count: int, limit: int, budget: int) -> str:
    """'over' past the guard's budget, 'early' from the early-warning share, else 'ok'."""
    if count > budget:
        return "over"
    if count >= EARLY_WARNING_RATIO * limit:
        return "early"
    return "ok"


def annotation(measurement: dict) -> str:
    count, limit, budget = (int(measurement[k]) for k in ("count", "limit", "budget"))
    level = classify(count, limit, budget)
    summary = (f"{count} scope-touching revisions of a {limit} limit "
               f"({count / limit:.0%}); the gate fails past {budget}")
    if level == "over":
        return f"::warning title={TITLE}::{summary}. The required gate is failing now: {REPIN}."
    if level == "early":
        return (f"::warning title={TITLE}::{summary}, {budget - count} revisions from now. "
                f"Schedule the re-pin: {REPIN}.")
    return f"::notice title={TITLE}::{summary}."


ISSUE_MARKER = "<!-- a2t-scope-history-headroom-tracking -->"

# Runs `gh api <args>` (with optional stdin) and returns the parsed JSON reply.
GhApi = Callable[[list[str], "str | None"], object]


def gh_api(args: list[str], stdin: str | None = None) -> object:
    proc = subprocess.run(["gh", "api", *args], input=stdin, capture_output=True,
                          text=True, timeout=60, env=os.environ.copy())
    if proc.returncode != 0:
        raise RuntimeError(f"gh api {' '.join(args[:3])} failed: {proc.stderr.strip()[:300]}")
    return json.loads(proc.stdout) if proc.stdout.strip() else None


def issue_title(count: int, limit: int) -> str:
    return (f"{TITLE}: {count}/{limit} scope-touching revisions; "
            "re-pin A2T_SCOPE_HISTORY_BASE")


def issue_body(measurement: dict) -> str:
    count, limit, budget = (int(measurement[k]) for k in ("count", "limit", "budget"))
    return (f"The A2T scope-history window holds **{count}** scope-touching revisions of a "
            f"**{limit}** limit ({count / limit:.0%}). `gpu-trace-overhead-acceptance-selftest` "
            f"fails every gate run once the count passes **{budget}**, "
            f"{max(budget - count, 0)} revisions from now.\n\n"
            f"Fix: {REPIN}. Pick the commit with the selftest's own counting "
            "(`_a2t_scope_touching_revision_ids` in "
            "`tools/scripts/gpu_trace_overhead_acceptance.py`) and do not move "
            "`A2T_SCOPE_BASE`.\n\n"
            "The push-to-main Build and Test run keeps this issue and closes it once a "
            "re-pin brings the count back under the early-warning share.\n\n"
            f"{ISSUE_MARKER}\n")


def reconcile_issue(measurement: dict, repo: str, api: GhApi = gh_api) -> str:
    """Open/update or close the one tracking issue; returns the action taken."""
    count, limit, budget = (int(measurement[k]) for k in ("count", "limit", "budget"))
    firing = classify(count, limit, budget) != "ok"
    found = api(["--method", "GET", "/search/issues", "-f",
                 f'q=repo:{repo} is:issue in:title "{TITLE}"'], None) or {}
    mine = [i for i in (found.get("items") or [])
            if ISSUE_MARKER in (i.get("body") or "")]
    # One issue: an open one wins over closed duplicates, then the newest.
    mine.sort(key=lambda i: (i.get("state") == "open", i.get("number", 0)), reverse=True)
    issue = mine[0] if mine else None
    if firing:
        fields = {"title": issue_title(count, limit), "body": issue_body(measurement)}
        if issue is None:
            api(["-X", "POST", f"/repos/{repo}/issues", "--input", "-"], json.dumps(fields))
            return "opened"
        api(["-X", "PATCH", f"/repos/{repo}/issues/{issue['number']}", "--input", "-"],
            json.dumps({**fields, "state": "open"}))
        return f"updated #{issue['number']}"
    if issue is not None and issue.get("state") == "open":
        api(["-X", "POST", f"/repos/{repo}/issues/{issue['number']}/comments", "-f",
             f"body=Headroom recovered: {count} of {limit} ({count / limit:.0%}). Closing."], None)
        api(["-X", "PATCH", f"/repos/{repo}/issues/{issue['number']}",
             "-f", "state=closed", "-f", "state_reason=completed"], None)
        return f"closed #{issue['number']}"
    return "none"


def main(argv: list[str]) -> int:
    args = list(argv[1:])
    repo = None
    reconcile = "--reconcile-issue" in args
    if reconcile:
        args.remove("--reconcile-issue")
        if "--repo" in args:
            i = args.index("--repo")
            repo = args[i + 1] if i + 1 < len(args) else None
            del args[i:i + 2]
    if len(args) != 1 or (reconcile and not repo):
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 0
    path = Path(args[0])
    try:
        measurement = json.loads(path.read_text(encoding="utf-8"))
        line = annotation(measurement)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"::warning title={TITLE}::A2T scope-history headroom was not measured "
              f"({type(error).__name__}: {error}); the window can fill without notice.")
        return 0
    print(line)
    if reconcile:
        try:
            print(f"a2t headroom tracking issue: {reconcile_issue(measurement, repo)}")
        except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as error:
            print(f"::warning title={TITLE}::could not reconcile the tracking issue: {error}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
