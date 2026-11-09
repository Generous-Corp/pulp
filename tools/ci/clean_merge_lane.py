#!/usr/bin/env python3
"""Choose the macOS lane for a pull-request head that is a clean merge of main.

A head whose only change is a conflict-free merge of the base branch into a
head that already passed the full macOS lane takes the configure-only contract
lane (configure, `pr-fast`, `pr-guard`) instead of the full native lane. The
pull-request head is never the proof of main: under the MERGE queue the
merge-group head is the commit that lands, and its `macos` job runs the full
suite. Decisions contract row 31 holds the rule; this module is its only
implementation.

The lane is `clean-merge` only when EVERY input below is read and agrees;
anything missing, unreadable or different yields `full` with a reason naming it:

- the event is `pull_request` `synchronize`, and `before` (the previous head)
  is a full commit id;
- the head has exactly two parents, one of them `before`, the other a
  different commit on the base branch's first-parent chain as fetched now;
- `git merge-tree --write-tree <before> <base parent>` exits 0 (no conflict)
  and its tree is the head's tree, so the head carries no hand resolution;
- the newest `macos` check run on `before` that this repository's `build.yml`
  produced on a same-repository `pull_request` run concluded `success`, its
  job's `Build` step succeeded, its `shipyard-test-tier` notice is `fast` or
  `full` (the full lane), and it is not itself a clean-merge or contract lane
  (no chaining).

A head's own `build.yml` runs this module, so it is a measurement on the head,
never a copied verdict: nothing here republishes the previous head's result.

Modes (repository variable PULP_CLEAN_MERGE_LANE_MODE): `shadow` (the default
when unset) decides and annotates `would_take` but runs the full lane;
`enforce` takes the lane; `off` decides nothing.

Annotation (`::notice title=pulp-clean-merge-lane::`), schema
`pulp-clean-merge-lane/v1`, verdicts `would_take | took | full`.

Usage:
    clean_merge_lane.py decide --repo . --repository O/R --head SHA --before SHA
        --base-ref main --action synchronize [--mode shadow] [--github-output F]
    clean_merge_lane.py report --repository O/R --since YYYY-MM-DD
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import subprocess
import sys
import urllib.parse
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

SCHEMA = "pulp-clean-merge-lane/v1"
TIER_SCHEMA = "shipyard-test-tier/v1"
NOTE_TITLE = "pulp-clean-merge-lane"
TIER_TITLE = "shipyard-test-tier"
MODES = ("shadow", "enforce", "off")
DEFAULT_MODE = "shadow"
WORKFLOW_PATH = ".github/workflows/build.yml"
CHECK_NAME = "macos"
ACTIONS_APP = "github-actions"
BUILD_STEP = "Build"
# The tiers a pull-request head's full lane reports: `fast` (built, fast tier
# plus affected tests) or `full` (the whole suite).
FULL_LANE_TIERS = ("fast", "full")
BASE_REF_LOCAL = "refs/pulp-ci/clean-merge/base"
GIT_TIMEOUT = 180
# Steps of the full lane whose failure on a `would_take` head means the lane
# would have hidden a red (decisions contract row 31's shadow-window control).
# A `pr-fast` red does not count: the clean-merge lane runs that tier.
FULL_LANE_PROOF_STEPS = (
    "Build",
    "Test what this pull request reaches (pull request head)",
    "Test installed SDK capability contract (affected changes)",
    "Install and publish exact GPU-audio SDK (macOS ARM64)",
)
ENFORCE_AFTER_DAYS = 7
ENFORCE_AFTER_HEADS = 50

_SHA = re.compile(r"^[0-9a-f]{40}$")
_RUN_JOB = re.compile(r"/actions/runs/(\d+)/job/(\d+)")


def is_sha(value: Optional[str]) -> bool:
    return bool(value) and bool(_SHA.match(value or "")) and set(value or "") != {"0"}


def _full(reason: str) -> Dict[str, Any]:
    return {"lane": "full", "reason": reason, "carried_from": None}


def decide(
    *,
    action: str,
    before: Optional[str],
    head: Optional[str],
    head_parents: Optional[List[str]],
    head_tree: Optional[str],
    on_base_chain: Optional[Dict[str, bool]],
    merge_tree: Optional[Dict[str, Any]],
    previous: Optional[List[Dict[str, Any]]],
    repository: str,
) -> Dict[str, Any]:
    """The lane for one head. Pure: every input is already read.

    `on_base_chain` maps a commit to whether it is on the base branch's
    first-parent chain (None when the chain could not be read). `merge_tree`
    is {"exit": int, "tree": str} for `git merge-tree --write-tree before
    <base parent>`. `previous` lists the `macos` check runs on `before`, each
    {"check_run_id", "app", "status", "conclusion", "run": {...} | None,
    "steps": {name: conclusion} | None, "tiers": [str], "lane_verdicts": [str]}.
    """
    if action != "synchronize":
        return _full(f"event action {action or 'unknown'!r} is not synchronize")
    if not is_sha(before):
        return _full("no previous head (github.event.before) to carry from")
    if not is_sha(head):
        return _full("the head commit id is unreadable")
    if head_parents is None:
        return _full("the head's parents could not be read")
    if len(head_parents) != 2:
        return _full(f"the head has {len(head_parents)} parents, not 2")
    if before not in head_parents:
        return _full("the previous head is not a parent of this head (rewritten history)")
    others = [p for p in head_parents if p != before]
    if len(others) != 1:
        return _full("the head merges the previous head into itself")
    other = others[0]
    if on_base_chain is None or other not in on_base_chain:
        return _full("the base branch's first-parent chain could not be read")
    if not on_base_chain[other]:
        return _full(f"the merged parent {other[:12]} is not on the base branch's first-parent chain")
    if merge_tree is None or not isinstance(merge_tree.get("exit"), int):
        return _full("git merge-tree could not be run")
    if merge_tree["exit"] != 0:
        return _full("git merge-tree reports a conflict, so the head carries a resolution")
    if not head_tree or merge_tree.get("tree") != head_tree:
        return _full("the head's tree differs from git merge-tree of its parents")
    if previous is None:
        return _full("the previous head's macos checks could not be read")

    trusted = []
    for check in previous:
        run = check.get("run")
        if check.get("app") != ACTIONS_APP:
            continue
        if run is None:
            return _full(f"the workflow run of macos check run {check.get('check_run_id')} could not be read")
        if (run.get("path") == WORKFLOW_PATH and run.get("event") == "pull_request"
                and run.get("repository") == repository
                and run.get("head_repository") == repository
                and run.get("head_sha") == before):
            trusted.append(check)
    if not trusted:
        return _full("the previous head has no macos check from this repository's build.yml pull_request runs")
    latest = max(trusted, key=lambda c: int(c.get("check_run_id") or 0))
    if latest.get("status") != "completed" or latest.get("conclusion") != "success":
        return _full(f"the previous head's latest macos check is {latest.get('status')}/{latest.get('conclusion')}, not a success")
    steps = latest.get("steps")
    if steps is None:
        return _full("the previous head's macos job steps could not be read")
    if steps.get(BUILD_STEP) != "success":
        return _full(f"the previous head's macos job did not build (Build: {steps.get(BUILD_STEP, 'absent')}), so it was not the full lane")
    tiers = list(latest.get("tiers") or [])
    if not tiers or any(t not in FULL_LANE_TIERS for t in tiers):
        return _full(f"the previous head's macos tier is {tiers or 'absent'}, not the full lane")
    if "took" in (latest.get("lane_verdicts") or []):
        return _full("the previous head's green was itself a clean-merge lane (no chaining)")
    return {"lane": "clean-merge",
            "reason": f"a clean merge of {other[:12]} into {before[:12]}, whose full macos lane passed",
            "carried_from": before}


def verdict_for(mode: str, decision: Dict[str, Any]) -> str:
    if decision["lane"] != "clean-merge":
        return "full"
    return "took" if mode == "enforce" else "would_take"


def note(mode: str, verdict: str, decision: Dict[str, Any], head: Optional[str],
         before: Optional[str]) -> str:
    record = {"schema": SCHEMA, "mode": mode, "verdict": verdict,
              "reason": decision["reason"], "carried_from": decision.get("carried_from"),
              "head": head if is_sha(head) else None,
              "before": before if is_sha(before) else None}
    return f"::notice title={NOTE_TITLE}::{json.dumps(record, sort_keys=True)}"


# --- gathering (impure) ---------------------------------------------------

def _git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=str(repo), capture_output=True, text=True,
                          encoding="utf-8", timeout=GIT_TIMEOUT, check=False)


def gather_git(repo: Path, head: str, before: str, base_ref: str,
               remote: str = "origin") -> Dict[str, Any]:
    """The git inputs of decide(); a field that cannot be read is None."""
    out: Dict[str, Any] = {"head_parents": None, "head_tree": None,
                           "on_base_chain": None, "merge_tree": None}
    if _git(repo, "rev-parse", "--is-shallow-repository").stdout.strip() != "false":
        return out  # a shallow history cannot prove parents or the chain
    if not is_sha(head) or not is_sha(before):
        return out
    parents = _git(repo, "rev-list", "--parents", "-n", "1", head)
    if parents.returncode != 0:
        return out
    fields = parents.stdout.split()
    if not fields or fields[0] != head:
        return out
    out["head_parents"] = fields[1:]
    tree = _git(repo, "rev-parse", f"{head}^{{tree}}")
    if tree.returncode == 0 and is_sha(tree.stdout.strip()):
        out["head_tree"] = tree.stdout.strip()
    others = [p for p in out["head_parents"] if p != before]
    if len(others) != 1 or before not in out["head_parents"]:
        return out
    other = others[0]
    if not re.match(r"^[A-Za-z0-9._/-]+$", base_ref or ""):
        return out
    fetch = _git(repo, "fetch", "--no-tags", "--quiet", remote,
                 f"+refs/heads/{base_ref}:{BASE_REF_LOCAL}")
    if fetch.returncode == 0:
        chain = _git(repo, "rev-list", "--first-parent", BASE_REF_LOCAL)
        if chain.returncode == 0:
            out["on_base_chain"] = {other: other in set(chain.stdout.split())}
    merged = _git(repo, "merge-tree", "--write-tree", before, other)
    lines = merged.stdout.split()
    if merged.returncode in (0, 1) and lines and is_sha(lines[0]):
        out["merge_tree"] = {"exit": merged.returncode, "tree": lines[0]}
    return out


Fetch = Callable[[str, str], Any]


def _fetch_json(url: str, token: str) -> Any:
    here = str(Path(__file__).resolve().parent.parent / "scripts")
    if here not in sys.path:
        sys.path.insert(0, here)
    import protected_merge_receipt  # type: ignore
    return protected_merge_receipt.api_json(url, token)


def _api(repository: str, path: str) -> str:
    return f"https://api.github.com/repos/{repository}/{path}"


def _annotations(repository: str, token: str, check_run_id: int, fetch: Fetch) -> List[Dict[str, Any]]:
    page = fetch(_api(repository, f"check-runs/{check_run_id}/annotations?per_page=100"), token)
    return page if isinstance(page, list) else []


def _tiers_and_verdicts(annotations: List[Dict[str, Any]]):
    tiers, verdicts = [], []
    for item in annotations:
        title, message = item.get("title"), item.get("message") or ""
        if title not in (TIER_TITLE, NOTE_TITLE):
            continue
        try:
            record = json.loads(message)
        except (json.JSONDecodeError, TypeError):
            record = {}
        if title == TIER_TITLE:
            tiers.append(str(record.get("tier", "unreadable")))
        else:
            verdicts.append(str(record.get("verdict", "unreadable")))
    return tiers, verdicts


def gather_previous(repository: str, token: str, before: str,
                    fetch: Optional[Fetch] = None) -> List[Dict[str, Any]]:
    """The `macos` check runs on `before`, with what decide() needs of each.
    Raises on any API failure; the caller decides `full`."""
    fetch = fetch or _fetch_json
    query = urllib.parse.urlencode({"check_name": CHECK_NAME, "per_page": 100})
    page = fetch(_api(repository, f"commits/{before}/check-runs?{query}"), token)
    checks = []
    for run in (page or {}).get("check_runs", []):
        if run.get("name") != CHECK_NAME:
            continue
        entry: Dict[str, Any] = {
            "check_run_id": run.get("id"), "status": run.get("status"),
            "conclusion": run.get("conclusion"),
            "app": (run.get("app") or {}).get("slug"),
            "run": None, "steps": None, "tiers": [], "lane_verdicts": [],
        }
        match = _RUN_JOB.search(run.get("details_url") or run.get("html_url") or "")
        if match and entry["app"] == ACTIONS_APP:
            run_id, job_id = match.group(1), match.group(2)
            wr = fetch(_api(repository, f"actions/runs/{run_id}"), token)
            entry["run"] = {
                "path": (wr.get("path") or "").split("@", 1)[0],
                "event": wr.get("event"),
                "repository": (wr.get("repository") or {}).get("full_name"),
                "head_repository": (wr.get("head_repository") or {}).get("full_name"),
                "head_sha": wr.get("head_sha"),
            }
            job = fetch(_api(repository, f"actions/jobs/{job_id}"), token)
            entry["steps"] = {s.get("name"): s.get("conclusion") for s in job.get("steps") or []}
            entry["tiers"], entry["lane_verdicts"] = _tiers_and_verdicts(
                _annotations(repository, token, int(entry["check_run_id"]), fetch))
        checks.append(entry)
    return checks


def cmd_decide(a: argparse.Namespace) -> int:
    """Decide, annotate, and write `decision`, `lane`, `carried_from` and
    `mode` for the workflow. Every failure is `full`; this never fails."""
    mode = a.mode if a.mode in MODES else DEFAULT_MODE
    if a.mode and a.mode not in MODES:
        print(f"clean-merge lane: unknown mode {a.mode!r}; using {DEFAULT_MODE}", file=sys.stderr)
    decision = _full("mode is off")
    if mode != "off":
        try:
            git_inputs = gather_git(Path(a.repo), a.head, a.before, a.base_ref)
            previous: Optional[List[Dict[str, Any]]] = None
            if a.action == "synchronize" and is_sha(a.before):
                try:
                    if not a.token:
                        raise RuntimeError("no token")
                    previous = gather_previous(a.repository, a.token, a.before)
                except Exception as exc:  # noqa: BLE001 - an unread check is `full`
                    print(f"clean-merge lane: previous checks unreadable ({type(exc).__name__})",
                          file=sys.stderr)
            decision = decide(action=a.action, before=a.before, head=a.head,
                              previous=previous, repository=a.repository, **git_inputs)
        except Exception as exc:  # noqa: BLE001 - the decision is never the gate
            decision = _full(f"the decision failed ({type(exc).__name__}: {exc})")
    verdict = verdict_for(mode, decision)
    if mode != "off":
        print(note(mode, verdict, decision, a.head, a.before))
    lane = "clean-merge" if verdict == "took" else "full"
    carried = decision.get("carried_from") if is_sha(decision.get("carried_from")) else ""
    print(f"clean-merge lane: mode={mode} decision={decision['lane']} lane={lane} ({decision['reason']})")
    if a.github_output:
        with open(a.github_output, "a", encoding="utf-8") as fh:
            fh.write(f"decision={decision['lane']}\n")
            fh.write(f"lane={lane}\n")
            fh.write(f"carried_from={carried}\n")
            fh.write(f"mode={mode}\n")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary and verdict != "full":
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(f"- clean-merge lane ({mode}): **{verdict}**, {decision['reason']}\n")
    return 0


# --- the shadow-window report ---------------------------------------------

def report(repository: str, token: str, since: str, fetch: Optional[Fetch] = None,
           today: Optional[_dt.date] = None) -> Dict[str, Any]:
    """Row 31's enforcement figures from the `macos` job annotations of
    build.yml pull_request runs created on or after `since`."""
    fetch = fetch or _fetch_json
    counts = {"would_take": 0, "took": 0, "full": 0}
    would_take_failed: List[Dict[str, Any]] = []
    took_heads: List[str] = []
    page_no = 1
    while True:
        query = urllib.parse.urlencode({"event": "pull_request", "created": f">={since}",
                                        "per_page": 100, "page": page_no})
        page = fetch(_api(repository, f"actions/workflows/build.yml/runs?{query}"), token)
        runs = (page or {}).get("workflow_runs", [])
        for run in runs:
            jobs = fetch(_api(repository, f"actions/runs/{run['id']}/jobs?filter=latest&per_page=100"), token)
            for job in (jobs or {}).get("jobs", []):
                if job.get("name") != CHECK_NAME:
                    continue
                check_url = job.get("check_run_url") or ""
                check_id = check_url.rsplit("/", 1)[-1]
                if not check_id.isdigit():
                    continue
                _, verdicts = _tiers_and_verdicts(_annotations(repository, token, int(check_id), fetch))
                for verdict in verdicts:
                    if verdict not in counts:
                        continue
                    counts[verdict] += 1
                    if verdict == "would_take":
                        failed = [s.get("name") for s in job.get("steps") or []
                                  if s.get("name") in FULL_LANE_PROOF_STEPS
                                  and s.get("conclusion") == "failure"]
                        if failed:
                            would_take_failed.append({"run_id": run["id"], "head_sha": run.get("head_sha"),
                                                      "failed_steps": failed})
                    elif verdict == "took":
                        took_heads.append(run.get("head_sha") or "")
        if len(runs) < 100:
            break
        page_no += 1
    today = today or _dt.datetime.now(_dt.timezone.utc).date()
    days = (today - _dt.date.fromisoformat(since)).days
    window_met = days >= ENFORCE_AFTER_DAYS or counts["would_take"] >= ENFORCE_AFTER_HEADS
    return {"schema": SCHEMA, "since": since, "days": days, **counts,
            "would_take_failed": would_take_failed, "took_heads": took_heads,
            "ready_to_enforce": window_met and counts["would_take"] > 0 and not would_take_failed}


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decide")
    d.add_argument("--repo", default=".")
    d.add_argument("--repository", required=True)
    d.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    d.add_argument("--head", default="")
    d.add_argument("--before", default="")
    d.add_argument("--base-ref", default="main")
    d.add_argument("--action", default="")
    d.add_argument("--mode", default=DEFAULT_MODE)
    d.add_argument("--github-output", default="")
    r = sub.add_parser("report")
    r.add_argument("--repository", required=True)
    r.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", "") or os.environ.get("GH_TOKEN", ""))
    r.add_argument("--since", required=True, help="YYYY-MM-DD, the first day of the window")
    a = ap.parse_args(argv)
    if a.cmd == "decide":
        return cmd_decide(a)
    if not a.token:
        print("report: no token (GITHUB_TOKEN or GH_TOKEN)", file=sys.stderr)
        return 2
    print(json.dumps(report(a.repository, a.token, a.since), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
