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

Two rules keep the verdict from resting on evidence it never read:

- "passes on main" is judged from main's TIP by its required gate job
  (`base_poison_detector.observe_main`), never from a whole run's conclusion,
  which an advisory leg can turn red. A tip whose gate did not execute the
  suite, a red gate whose failing-test list cannot be downloaded, and a red
  gate that named no test are all "main unknown", and unknown never
  exonerates. The one widening: when the tip's gate did not execute the suite,
  `--main-lookback N` (default 10; 0 = tip only) walks first-parent back, within
  24 h of the tip, to the newest commit whose gate did, and the annotation
  records that commit (`main_read_sha`), its distance (`main_distance`) and
  `main_source`. The skipped commits between were reuse or skip-safe groups, so
  the gate's own contract is that they did not change what the suite reads;
  this shadow does not re-verify that, and the annotation names the commit so
  a reader can.
- A deterministic test is never a flake. The `pr-fast` label is the static
  contract tier (tests that read the tree and must be deterministic to join
  it), and `tools/ci/drift_fast.json` names the other tree-reading drift
  checks; a failure of either is a real defect, so neither is eligible.
  Repetition cannot be the discriminator here: the gate runs ctest with
  `--repeat until-pass:2`, so every test in the JUnit report already failed
  both attempts.

Proxies: merge-group `macos` failures whose failing tests are ALL
would-exonerate ÷ merge-group failures; `main_evidence_read` ÷ exonerations,
which must be 1; controls: failures with a unique cause (no other head) > 0,
and the base-red streak count unchanged (condition (b) makes a red main
non-exonerable).
"""
from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import os
import sys
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import base_poison_detector as bpd  # noqa: E402

SCHEMA = "pulp-flake-exoneration-shadow/v1"
MIN_OTHER_HEADS = 2
MAIN_LOOKBACK = 10
DETERMINISTIC_LABEL = "pr-fast"
DRIFT_FAST_LIST = Path(__file__).resolve().parents[2] / "tools/ci/drift_fast.json"


def junit_failures(path: Path) -> list[str]:
    return sorted(junit_failure_labels(path))


def junit_failure_labels(path: Path) -> dict[str, set[str]]:
    """Failing test name -> its ctest labels (`cmake_labels` properties)."""
    root = ET.parse(path).getroot()
    out: dict[str, set[str]] = {}
    for case in root.iter("testcase"):
        name = case.get("name") or ""
        if not name or not any(c.tag in ("failure", "error") for c in case):
            continue
        labels = out.setdefault(name, set())
        for prop in case.iterfind("properties/property"):
            if prop.get("name") == "cmake_labels":
                labels.update(x for x in (prop.get("value") or "").split(";") if x)
    return out


def drift_fast_tests(path: Path = DRIFT_FAST_LIST) -> set[str]:
    """Registrations drift-fast names individually: tree-reading, deterministic."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return set()
    return {t.get("name", "") for t in data.get("tests", []) if isinstance(t, dict)} - {""}


def deterministic_tests(labels: dict[str, set[str]], drift_names: set[str]) -> set[str]:
    return {name for name, tags in labels.items()
            if DETERMINISTIC_LABEL in tags or name in drift_names}


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


def read_failing_tests(repo: str, run_id: str, key: str = "macos") -> tuple[str, ...] | None:
    """The failing tests a run's ctest-logs artifact names, or None when unreadable.

    `base_poison_detector.artifact_failing_tests` answers `()` both for "read,
    nothing failed" and for "could not read"; exoneration must not treat the
    second as a pass on main.
    """
    raw = bpd.gh(
        f"repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100",
        f'[.artifacts[]|select(.name=="{bpd.CTEST_ARTIFACT_PREFIX}{key}" and (.expired|not))]|.[0].id',
    )
    if not raw or not raw.strip().isdigit():
        return None
    blob = bpd.gh_bytes(f"repos/{repo}/actions/artifacts/{raw.strip()}/zip")
    if not blob:
        return None
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            with archive.open(bpd.LAST_TESTS_FAILED_MEMBER) as member:
                return bpd.parse_last_tests_failed(member.read().decode("utf-8", "replace"))
    except (zipfile.BadZipFile, KeyError):
        return None


@dataclass(frozen=True)
class MainEvidence:
    """What main's tip says about a failing test, and whether it was actually read."""

    run_id: str | None
    conclusion: str  # "success" | "failure" | "unknown"
    failing: frozenset[str] = frozenset()
    reason: str = ""
    # The commit whose gate was read, and how many first-parent steps back from
    # main's tip it is (0 = the tip itself). Empty when nothing was read.
    read_sha: str = ""
    distance: int = 0
    # "tip" (the tip's own gate), "tree-identity" (an earlier group that built
    # the tip's tree; distance does not apply), or "first-parent-ancestor".
    source: str = ""

    @property
    def read(self) -> bool:
        return self.conclusion in ("success", "failure")


def _judge_failing(obs, failing, where) -> MainEvidence:
    if failing is None:
        return MainEvidence(obs.run_id, "unknown", reason="main's failing-test list could not be read", **where)
    if not failing:
        return MainEvidence(obs.run_id, "unknown", reason="main's gate failed without naming a test", **where)
    return MainEvidence(obs.run_id, "failure", frozenset(failing), **where)


def first_parent_step(repo: str, sha: str) -> tuple[str, dt.datetime | None] | None:
    """(first parent sha, this commit's committer time), or None when unreadable."""
    raw = bpd.gh(f"repos/{repo}/commits/{sha}",
                 "[(.parents[0].sha // \"\"), .commit.committer.date]|@tsv")
    if not raw or "\t" not in raw:
        return None
    parent, when = raw.split("\t", 1)
    try:
        stamp = dt.datetime.fromisoformat(when.strip().replace("Z", "+00:00"))
    except ValueError:
        stamp = None
    return parent.strip(), stamp


def main_evidence(repo: str, observe_fn=None, failing_fn=None, lookback: int = 0,
                  lookback_hours: int = 24, tip_fn=None, step_fn=None, head_obs_fn=None) -> MainEvidence:
    """main's tip judged by its required gate job; unknown unless actually read.

    When the tip's gate did not execute the suite (a receipt reuse, a skip-safe
    group) and `lookback` > 0, walk FIRST-PARENT back, at most `lookback`
    commits and `lookback_hours` hours from the tip's commit time, to the
    newest commit whose gate did execute it, and judge from that one. The
    commit read and its distance are recorded. Every other unread case stays
    unknown; a first-parent chain is followed one parent link at a time, so a
    commit outside the tip's first-parent ancestry is never read.
    """
    observe_fn = observe_fn or (lambda r: bpd.observe_main(r)[0])
    failing_fn = failing_fn or read_failing_tests
    obs = observe_fn(repo)
    if obs is None:
        return MainEvidence(None, "unknown", reason="no merge-group run observed main's tip")
    if obs.is_evidence:
        return _judge_obs(repo, obs, failing_fn, "", 0)
    reason = f"main's gate did not execute the suite ({obs.execution}, {obs.conclusion or 'running'})"
    if lookback > 0 and obs.execution != bpd.EXECUTED:
        found = _walk_first_parent(repo, lookback, lookback_hours, tip_fn, step_fn, head_obs_fn)
        if found is None:
            return MainEvidence(obs.run_id, "unknown",
                                reason=f"{reason}; no commit within {lookback} first-parent commits / "
                                       f"{lookback_hours} h ran it")
        sha, distance, earlier = found
        return _judge_obs(repo, earlier, failing_fn, sha, distance)
    return MainEvidence(obs.run_id, "unknown", reason=reason)


def _judge_obs(repo, obs, failing_fn, sha, distance) -> MainEvidence:
    if sha:
        source = "first-parent-ancestor"
    else:
        source = "tree-identity" if obs.source == bpd.SOURCE_TREE_IDENTITY else "tip"
    where = {"read_sha": sha or obs.head_sha, "distance": distance, "source": source}
    if obs.passed:
        return MainEvidence(obs.run_id, "success", **where)
    return _judge_failing(obs, failing_fn(repo, obs.run_id), where)


def _walk_first_parent(repo, lookback, hours, tip_fn, step_fn, head_obs_fn):
    """(sha, distance, observation) of the newest first-parent ancestor of main's
    tip whose gate executed the suite, within the bounds; None otherwise."""
    tip_fn = tip_fn or (lambda r: bpd.main_head(r)[0])
    step_fn = step_fn or first_parent_step
    head_obs_fn = head_obs_fn or bpd.observe_main_by_head_sha
    tip = tip_fn(repo)
    first = step_fn(repo, tip) if tip else None
    if first is None or first[1] is None:
        return None
    parent, tip_time = first
    for distance in range(1, lookback + 1):
        if not parent:
            return None
        # `parent` is the first parent of the commit before it, never any other
        # ancestor; its own step supplies its time and the next first parent.
        step = step_fn(repo, parent)
        if step is None or step[1] is None or tip_time - step[1] > dt.timedelta(hours=hours):
            return None
        seen = head_obs_fn(repo, parent)
        if seen is not None and seen.is_evidence:
            return parent, distance, seen
        parent = step[0]
    return None


def decide(failing: list[str], by_head: dict[str, set[str]], main: MainEvidence,
           deterministic: set[str] = frozenset(), min_other_heads: int = MIN_OTHER_HEADS) -> dict:
    """Pure verdict: which failing tests would be exonerated and why not otherwise."""
    verdicts = {}
    for name in failing:
        heads = by_head.get(name, set())
        passes_on_main = main.conclusion == "success" or (main.conclusion == "failure"
                                                          and name not in main.failing)
        is_deterministic = name in deterministic
        would = len(heads) >= min_other_heads and passes_on_main and not is_deterministic
        reason = ("OCCURS_ON_OTHER_CLS" if would else
                  "deterministic test (static contract or drift check)" if is_deterministic else
                  "fails on main too" if main.conclusion == "failure" and not passes_on_main else
                  f"main result unknown: {main.reason}" if not main.read else
                  f"only {len(heads)} other head(s)")
        verdicts[name] = {"would_exonerate": would, "other_heads": len(heads), "reason": reason,
                          "deterministic": is_deterministic, "main_evidence_read": main.read}
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
    ap.add_argument("--main-lookback", type=int, default=MAIN_LOOKBACK,
                    help="first-parent commits to walk back from main's tip when its gate did not "
                         "execute the suite (0 = tip only)")
    a = ap.parse_args(argv[1:])
    try:
        labels = junit_failure_labels(Path(a.junit))
    except (OSError, ET.ParseError) as exc:
        print(f"flake-exoneration shadow: no verdict, JUnit unreadable: {exc}", file=sys.stderr)
        return 2
    failing = sorted(labels)
    if not failing:
        print("flake-exoneration shadow: no failing test in the JUnit report; nothing to decide")
        return 0
    runs = recent_failed_runs(a.repo, a.hours, a.head)
    by_head = failures_by_head(a.repo, runs)
    main = main_evidence(a.repo, lookback=max(0, a.main_lookback))
    result = decide(failing, by_head, main, deterministic_tests(labels, drift_fast_tests()),
                    a.min_other_heads)
    result.update({"schema": SCHEMA, "mode": "shadow", "failing": len(failing), "window_hours": a.hours,
                   "runs_scanned": len(runs), "main_run_id": main.run_id, "main_conclusion": main.conclusion,
                   "main_evidence_read": main.read, "main_read_sha": main.read_sha,
                   "main_distance": main.distance, "main_source": main.source, "main_lookback": a.main_lookback, "main_reason": main.reason, "head": a.head})
    would = [n for n, v in result["tests"].items() if v["would_exonerate"]]
    print(f"flake-exoneration shadow: {len(failing)} failing test(s); would exonerate {len(would)} "
          f"(OCCURS_ON_OTHER_CLS: >= {a.min_other_heads} other heads in {a.hours} h and passing on main run "
          f"{main.run_id} [{main.conclusion}]); unique-cause {result['unique_cause']}; runs scanned {len(runs)}")
    print(f"::notice title=flake-exoneration-shadow::{json.dumps(result, sort_keys=True)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
