#!/usr/bin/env python3
"""Would this merge-group failure have been exonerated as a known flake? Shadow only.

Chromium's CQ retries a failing test with the patch, then re-runs it without
the patch, and records why a failure was not held against the CL
(ResultDB `ExonerationReason`: OCCURS_ON_MAINLINE, OCCURS_ON_OTHER_CLS).
Pulp's gate already has the with-patch retry (`ctest --repeat until-pass:2`)
and a base-red detector (`base_poison_detector.py`, OCCURS_ON_MAINLINE read
from history). What it lacks is OCCURS_ON_OTHER_CLS: seven single-cause
flakes in 40 h each failed twice on one VM and ejected a whole ALLGREEN batch
(25–40 gate-minutes plus every neighbour), although the same tests had been
failing on unrelated heads and passing on main.

This tool runs after a FAILED merge-group ctest and annotates, per failing
test, whether it WOULD have been exonerated:

    would_exonerate(test) := failed on >= 2 OTHER heads in the last N hours
                             AND passes on main's latest validated tree

It changes no outcome: it prints `pulp-flake-exoneration-shadow/v1` and a
notice, and exits 0 whatever it finds (2 only when it cannot read its
inputs). `base_poison_detector.py` documents why cross-batch corroboration
alone was measured UNSAFE as a queue action (three disjoint batches once
shared a byte-identical failure whose head really was broken); this shadow
keeps that rule by (a) requiring a pass on main's latest tree and (b) acting
on nothing. The data it produces is what a later decision has to stand on.

Proxies: merge-group `macos` failures whose failing tests are ALL
would-exonerate ÷ merge-group failures; controls: failures with a unique
cause (no other head) > 0, and the base-red streak count unchanged
(condition (b) makes a red main non-exonerable).
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import base_poison_detector as bpd  # noqa: E402

SCHEMA = "pulp-flake-exoneration-shadow/v1"
MIN_OTHER_HEADS = 2


def junit_failures(path: Path) -> list[str]:
    root = ET.parse(path).getroot()
    return sorted({case.get("name") or "" for case in root.iter("testcase")
                   if any(c.tag in ("failure", "error") for c in case)} - {""})


def _created(run: dict) -> dt.datetime | None:
    try:
        return dt.datetime.fromisoformat(str(run.get("created_at", "")).replace("Z", "+00:00"))
    except ValueError:
        return None


def recent_failed_runs(repo: str, hours: int, this_head: str, now: dt.datetime | None = None,
                       runs_fn=None) -> list[dict]:
    """Completed Build runs (PR heads and merge groups) in the window whose head is not ours."""
    runs_fn = runs_fn or bpd._runs
    now = now or dt.datetime.now(dt.timezone.utc)
    since = now - dt.timedelta(hours=hours)
    out = []
    for event in ("pull_request", "merge_group"):
        for r in runs_fn(repo, event, 100):
            created = _created(r)
            if created and created >= since and r.get("head_sha") != this_head:
                out.append(r)
    return out


def failures_by_head(repo: str, runs: list[dict], failing_fn=None) -> dict[str, set[str]]:
    """test name → set of head SHAs (other runs) where it failed, from the ctest-logs artifact."""
    failing_fn = failing_fn or bpd.artifact_failing_tests
    table: dict[str, set[str]] = {}
    for r in runs:
        if r.get("conclusion") not in ("failure", "timed_out"):
            continue
        for name in failing_fn(repo, str(r["id"])):
            table.setdefault(name, set()).add(str(r.get("head_sha")))
    return table


def main_latest_result(repo: str, runs_fn=None, failing_fn=None) -> tuple[str | None, str, set[str]]:
    """(run id, conclusion, failing tests) of the latest completed merge_group run."""
    runs_fn = runs_fn or bpd._runs
    failing_fn = failing_fn or bpd.artifact_failing_tests
    groups = runs_fn(repo, "merge_group", 100)
    groups = sorted(groups, key=lambda r: r.get("created_at") or "", reverse=True)
    if not groups:
        return None, "unknown", set()
    latest = groups[0]
    conclusion = latest.get("conclusion") or "unknown"
    failing = set(failing_fn(repo, str(latest["id"]))) if conclusion != "success" else set()
    return str(latest["id"]), conclusion, failing


def decide(failing: list[str], by_head: dict[str, set[str]], main_conclusion: str,
           main_failing: set[str], min_other_heads: int = MIN_OTHER_HEADS) -> dict:
    """Pure verdict: which failing tests would be exonerated and why not otherwise."""
    verdicts = {}
    for name in failing:
        heads = by_head.get(name, set())
        passes_on_main = main_conclusion == "success" or (main_conclusion in ("failure", "timed_out")
                                                          and name not in main_failing)
        if main_conclusion not in ("success", "failure", "timed_out"):
            passes_on_main = False
        would = len(heads) >= min_other_heads and passes_on_main
        reason = ("OCCURS_ON_OTHER_CLS" if would else
                  "fails on main too" if not passes_on_main and main_conclusion in ("failure", "timed_out") else
                  "main result unknown" if not passes_on_main else
                  f"only {len(heads)} other head(s)")
        verdicts[name] = {"would_exonerate": would, "other_heads": len(heads), "reason": reason}
    return {"tests": verdicts,
            "exonerated_only": bool(failing) and all(v["would_exonerate"] for v in verdicts.values()),
            "unique_cause": sum(1 for v in verdicts.values() if v["other_heads"] == 0)}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--junit", required=True)
    ap.add_argument("--head", default=os.environ.get("GITHUB_SHA", ""))
    ap.add_argument("--hours", type=int, default=24)
    ap.add_argument("--min-other-heads", type=int, default=MIN_OTHER_HEADS)
    a = ap.parse_args(argv[1:])
    try:
        failing = junit_failures(Path(a.junit))
    except (OSError, ET.ParseError) as exc:
        print(f"flake-exoneration shadow: no verdict, JUnit unreadable: {exc}", file=sys.stderr)
        return 2
    if not failing:
        print("flake-exoneration shadow: no failing test in the JUnit report; nothing to decide")
        return 0
    runs = recent_failed_runs(a.repo, a.hours, a.head)
    by_head = failures_by_head(a.repo, runs)
    main_run, main_conclusion, main_failing = main_latest_result(a.repo)
    result = decide(failing, by_head, main_conclusion, main_failing, a.min_other_heads)
    result.update({"schema": SCHEMA, "mode": "shadow", "failing": len(failing), "window_hours": a.hours,
                   "runs_scanned": len(runs), "main_run_id": main_run, "main_conclusion": main_conclusion,
                   "head": a.head})
    would = [n for n, v in result["tests"].items() if v["would_exonerate"]]
    print(f"flake-exoneration shadow: {len(failing)} failing test(s); would exonerate {len(would)} "
          f"(OCCURS_ON_OTHER_CLS: >= {a.min_other_heads} other heads in {a.hours} h and passing on main run "
          f"{main_run} [{main_conclusion}]); unique-cause {result['unique_cause']}; runs scanned {len(runs)}")
    print(f"::notice title=flake-exoneration-shadow::{json.dumps(result, sort_keys=True)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
