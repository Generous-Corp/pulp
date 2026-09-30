#!/usr/bin/env python3
"""Replay test-result reuse policies over merge-queue history before any of them skips a test.

A reuse policy decides, for a merge group, which of its tests may be skipped
because an earlier PR-head run already proved them. Skipping is the
optimisation; a wrong skip lets a real failure merge. History already holds
both sides of every decision: the PR head's full-suite run and the merge
group's own run, which ran everything. So each policy can be scored offline
against what really happened, with no VM time, and a policy ships only when
that score says it would never have skipped a test that failed.

    reuse_policy_replay.py collect --since <ISO> [--until <ISO>] --out <corpus-dir>
    reuse_policy_replay.py score   --corpus <corpus-dir> --policy <name> [--json]
    reuse_policy_replay.py score   --scenarios <dir>        # named-scenario fixtures

`collect` is Pulp's adapter and lives in reuse_replay_collect.py; everything
in this module is project-neutral and reads only the corpus.

CORPUS (neutral JSONL; nothing here knows about GitHub or ctest)

  runs.jsonl        one record per run: run_kind (pr_head | merge_group), pr,
                    head_sha (the PR head), group_sha (merge groups), the commit
                    the job actually checked out and its parents, base_sha,
                    merge_tree, runner_image, ctest facts, receipt facts.
  tests/<run>.jsonl.gz
                    one record per test per run: test_id, executable, outcome
                    (pass | fail | timeout | notrun | skipped), attempts,
                    duration_s, source_key, output_key, runner_image.
  pairs.jsonl       one record per merge group: its PR head's candidate runs
                    (latest first, each with the files that differ between the
                    two checked-out trees), whether the group was stacked on an
                    unlanded entry, and the reuse decision the group recorded.

A run whose checked-out commit does not bind to its own head (a merge ref
whose second parent is not the head, a group that checked out another commit)
is REJECTED, at collect and again at score: keys and trees come from the
record's own checkout, never from something cached beside it.

METRICS (per policy)

  benefit      skipped test-seconds / group test-seconds per scored pair,
               median and p90 (ctest per-test durations, never wall time).
  false_skips  group tests whose FINAL outcome (after the gate's own
               `--repeat until-pass:2`) was fail or timeout and that the policy
               would have skipped, unless the flake-exoneration rule held for
               the test. MUST BE 0; a policy with any does not ship.
  flake_skips  skipped tests that failed and then passed on retry
               (attempts > 1), or failed and were exonerated. Reported
               separately; a skipped flake is fine.
  coverage     scored pairs the policy could evaluate / scored pairs. A pair
               whose records lack what the policy needs runs everything; it is
               never counted as a skip.

  A group whose build failed has no test outcomes: it is excluded from test
  scoring and reported as `build_failed`, and `build_failures_skipped` lists
  the ones a build-skipping policy would have skipped.

POLICIES

  whole-receipt  today's rule: reuse only when an eligible PR-head run checked
                 out the identical tree on the identical base.
  inert-drift    whole-receipt, plus: the trees differ only in files that
                 `classify_changes.py` classifies as not feeding the native
                 build (fail closed: an empty or unclassifiable drift runs),
                 and the group is not stacked on an unlanded entry.
  inert-drift-inputs
                 inert-drift, plus no drifted file is a declared input of a
                 script test (`test/ctest_script_inputs.json` at the group's
                 tree); a group whose tree has no such list is unevaluable.
  source-key, per-executable
                 tier-1 and tier-2 seams: registered, not implemented.
  none, key-match-reference
                 controls for the scenario fixtures: `none` skips nothing;
                 `key-match-reference` skips a test whose head and group
                 records carry the same output_key. Neither is a candidate.

Exit codes: 0 success; 1 `score` found a false skip or a scenario verdict
mismatch, or `collect` found no merge groups or no PR-head pairs (the
controls); 2 invalid input.
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import gzip
import json
import statistics
import sys
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import classify_changes  # noqa: E402

RUN_SCHEMA = "pulp-reuse-replay-run/v1"
TEST_SCHEMA = "pulp-reuse-replay-test/v1"
PAIR_SCHEMA = "pulp-reuse-replay-pair/v1"
SCENARIO_SCHEMA = "pulp-reuse-replay-scenario/v1"

FAIL_OUTCOMES = frozenset({"fail", "timeout"})
GREEN_CONCLUSIONS = ("success", "skipped", "neutral")
# A PR-head run is full-suite evidence only when it ran at least this share of
# the tests its merge group ran (the receipt issuer's own floor).
MIN_SELECTED_PERCENT = 80
SCRIPT_INPUTS_PATH = "test/ctest_script_inputs.json"


# --------------------------------------------------------------------------
# Records
# --------------------------------------------------------------------------

def validate_run(run: dict) -> str | None:
    """Why this run's checkout does not bind to its own head, or None.

    A merge group must have checked out its own merge commit, whose second
    parent is the PR head; a PR-head run checks out a merge ref whose second
    parent is the head. Anything else means the record describes a tree other
    than the one its head names (a stale checkout, a reused build dir)."""
    checkout = run.get("checkout_sha")
    parents = run.get("checkout_parents") or []
    if not checkout or len(parents) != 2:
        return "checkout unknown"
    if run.get("run_kind") == "merge_group" and checkout != run.get("group_sha"):
        return "group checked out a commit other than its merge commit"
    if parents[1] != run.get("head_sha"):
        return "checked-out tree does not contain the recorded head"
    if run.get("base_sha") != parents[0]:
        return "recorded base is not the checkout's first parent"
    if not run.get("merge_tree"):
        return "checkout tree unknown"
    return None


def receipt_eligible(run: dict, source: str = "derived", contexts_green: bool | None = None) -> bool:
    """Would this PR-head run have issued a reusable receipt?

    `derived`: it ran the full suite to completion with every test's final
    outcome green, and no required context on the head was red or unknown
    (`contexts_green`, as of the group's creation, when the pair records it).
    `observed`: the issuer said it published one."""
    if run.get("run_kind") != "pr_head" or validate_run(run):
        return False
    if source == "observed":
        return run.get("receipt_issued") is True
    ctest = run.get("ctest") or {}
    return bool(ctest.get("complete") and ctest.get("full_suite") is True
                and ctest.get("failed", 1) == 0
                and (contexts_green if contexts_green is not None
                     else run.get("required_contexts_green")) is True)


def image_compatible(a: dict, b: dict) -> bool:
    """A record without a runner image cannot match one with it."""
    return a.get("runner_image") == b.get("runner_image")


# --------------------------------------------------------------------------
# Corpus
# --------------------------------------------------------------------------

class Corpus:
    """Runs, pairs and per-run test records, from a directory or from memory."""

    def __init__(self, runs: Iterable[dict], pairs: Iterable[dict],
                 tests: dict[str, list[dict]] | None = None,
                 tests_dir: Path | None = None) -> None:
        self.runs = {str(r["run_id"]): r for r in runs}
        self.pairs = list(pairs)
        self._tests = {str(k): v for k, v in (tests or {}).items()}
        self._tests_dir = tests_dir

    @classmethod
    def load(cls, root: Path) -> "Corpus":
        runs = list(read_jsonl(root / "runs.jsonl"))
        pairs = list(read_jsonl(root / "pairs.jsonl"))
        return cls(runs, pairs, tests_dir=root / "tests")

    def tests(self, run_id: str) -> list[dict] | None:
        run_id = str(run_id)
        if run_id not in self._tests and self._tests_dir is not None:
            path = self._tests_dir / f"{run_id}.jsonl.gz"
            self._tests[run_id] = list(read_jsonl(path)) if path.exists() else None
        return self._tests.get(run_id)

    def run(self, run_id: Any) -> dict | None:
        return self.runs.get(str(run_id)) if run_id is not None else None


def read_jsonl(path: Path) -> Iterator[dict]:
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                yield json.loads(line)


def write_jsonl(path: Path, rows: Iterable[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    opener = gzip.open if path.suffix == ".gz" else open
    tmp = path.with_name(path.name + ".tmp")
    with opener(tmp, "wt", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
    tmp.replace(path)


# --------------------------------------------------------------------------
# Policies
# --------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Decision:
    """A policy's verdict for one pair.

    `evaluable` False means the records lacked what the policy needs, so the
    group runs everything. `skip_all` skips the build and every test;
    otherwise `skip` names the tests skipped (the build still runs)."""
    evaluable: bool
    reason: str
    skip_all: bool = False
    skip: frozenset = frozenset()

    def skips(self, test_id: str) -> bool:
        return self.skip_all or test_id in self.skip


RUN_ALL = "run everything"


def _unevaluable(reason: str) -> Decision:
    return Decision(False, f"{RUN_ALL}: {reason}")


class PolicyNotImplemented(RuntimeError):
    """A registered seam whose decision function does not exist yet."""


@dataclasses.dataclass(frozen=True)
class Policy:
    name: str
    decide: Callable[[dict, Corpus, dict], Decision] | None
    candidate: bool
    summary: str


def _group_and_heads(pair: dict, corpus: Corpus, opts: dict
                     ) -> tuple[dict | None, list[tuple[dict, dict]]]:
    """The group run and its eligible, valid head candidates, latest first."""
    group = corpus.run(pair.get("group_run_id"))
    source = opts.get("receipt_source", "derived")
    heads = []
    for cand in pair.get("heads") or []:
        run = corpus.run(cand.get("run_id"))
        if run is not None and receipt_eligible(run, source, cand.get("required_contexts_green")):
            heads.append((run, cand))
    return group, heads


def decide_whole_receipt(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    group, heads = _group_and_heads(pair, corpus, opts)
    if group is None or validate_run(group):
        return _unevaluable("group checkout unknown or rejected")
    if not heads:
        return _unevaluable("no eligible PR-head receipt")
    for run, _ in heads:
        if (run["base_sha"] == group["base_sha"] and run["merge_tree"] == group["merge_tree"]):
            if not image_compatible(run, group):
                return Decision(True, "runner image differs")
            return Decision(True, f"exact tree on exact base (head run {run['run_id']})", skip_all=True)
    return Decision(True, "base drift")


def _inert_drift(pair: dict, corpus: Corpus, opts: dict, check_inputs: bool) -> Decision:
    exact = decide_whole_receipt(pair, corpus, opts)
    if exact.skip_all or not exact.evaluable:
        return exact
    group, heads = _group_and_heads(pair, corpus, opts)
    run, cand = heads[0]
    if not image_compatible(run, group):
        return Decision(True, "runner image differs")
    stacked = pair.get("stacked")
    if stacked is None:
        return _unevaluable("stacking unknown")
    if stacked:
        return Decision(True, "stacked on an unlanded entry")
    files = cand.get("drift_files")
    if files is None:
        return _unevaluable("drift not computable")
    if not files:
        # Different trees with no differing file is contradictory data.
        return _unevaluable("empty drift between different trees")
    # Fail-closed: native_build_required is True for any file it does not
    # explicitly know to be inert.
    if classify_changes.native_build_required(list(files)):
        return Decision(True, "drift feeds the native build")
    if check_inputs:
        hits = cand.get("drift_declared_input_hits")
        if hits is None:
            return _unevaluable("no declared script inputs at the group tree")
        if hits:
            return Decision(True, "drift is a declared script-test input")
    return Decision(True, f"inert drift ({len(files)} files)", skip_all=True)


def decide_inert_drift(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    return _inert_drift(pair, corpus, opts, check_inputs=False)


def decide_inert_drift_inputs(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    return _inert_drift(pair, corpus, opts, check_inputs=True)


def decide_none(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    group = corpus.run(pair.get("group_run_id"))
    if group is None or validate_run(group):
        return _unevaluable("group checkout unknown or rejected")
    return Decision(True, "control: skips nothing")


def decide_key_match_reference(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    """Skip a test when an eligible-or-not PR-head run passed it on the first
    attempt under the same output key. Fixture-only: it exists so scenarios
    about key composition have a policy to score."""
    group = corpus.run(pair.get("group_run_id"))
    if group is None or validate_run(group):
        return _unevaluable("group checkout unknown or rejected")
    group_tests = corpus.tests(group["run_id"]) or []
    if any(t.get("key_inputs_clean") is False for t in group_tests):
        return _unevaluable("a key was built from an untracked or build-dir input")
    for cand in pair.get("heads") or []:
        run = corpus.run(cand.get("run_id"))
        if run is None or validate_run(run):
            continue
        head_tests = {t["test_id"]: t for t in corpus.tests(run["run_id"]) or []}
        if not head_tests:
            continue
        if not image_compatible(run, group):
            return Decision(True, "runner image differs")
        skip = set()
        for t in group_tests:
            h = head_tests.get(t["test_id"])
            if (h is None or h.get("outcome") != "pass" or h.get("attempts", 1) != 1
                    or not t.get("output_key") or t.get("output_key") != h.get("output_key")):
                continue
            if t.get("kind") == "script" and t.get("hermetic") is not True:
                continue
            skip.add(t["test_id"])
        return Decision(True, f"{len(skip)} keys matched", skip=frozenset(skip))
    return _unevaluable("no head test records")


POLICIES: dict[str, Policy] = {p.name: p for p in (
    Policy("whole-receipt", decide_whole_receipt, True,
           "identical tree on identical base (the live rule)"),
    Policy("inert-drift", decide_inert_drift, True,
           "whole-receipt, or drift classify_changes calls non-native, not stacked"),
    Policy("inert-drift-inputs", decide_inert_drift_inputs, True,
           "inert-drift, and no drifted file is a declared script-test input"),
    Policy("source-key", None, True,
           "tier 1: one source key over every test's inputs (not implemented)"),
    Policy("per-executable", None, True,
           "tier 2: output key per test executable (not implemented)"),
    Policy("none", decide_none, False, "control: skips nothing"),
    Policy("key-match-reference", decide_key_match_reference, False,
           "fixture control: equal output_key and a first-attempt head pass"),
)}


# --------------------------------------------------------------------------
# Scoring
# --------------------------------------------------------------------------

def pair_status(group: dict | None) -> str:
    """How a group's own run can be used as ground truth."""
    if group is None:
        return "no_group_record"
    if validate_run(group):
        return "rejected"
    if group.get("observed_decision") == "reuse":
        return "observed_reused"
    ctest = group.get("ctest") or {}
    if group.get("build_failed"):
        return "build_failed"
    if not ctest:
        return "no_suite"  # no native input, or the suite job never ran
    if not ctest.get("complete"):
        return "incomplete"
    return "scored"


def _percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(q * (len(ordered) - 1)))))
    return ordered[index]


def score(corpus: Corpus, policy_name: str, opts: dict | None = None) -> dict:
    """Score one policy over every pair of the corpus. Pure over the corpus."""
    opts = opts or {}
    policy = POLICIES.get(policy_name)
    if policy is None:
        raise KeyError(f"unknown policy {policy_name!r}; known: {', '.join(POLICIES)}")
    if policy.decide is None:
        raise PolicyNotImplemented(f"policy {policy_name!r} is a registered seam without a decision function")
    statuses: dict[str, int] = {}
    benefits: list[float] = []
    false_skips: list[dict] = []
    flake_skips: list[dict] = []
    build_failures_skipped: list[dict] = []
    evaluable = scored = skipped_groups = 0
    skipped_seconds = total_seconds = 0.0
    replay_vs_observed = {"both_reuse": 0, "replay_only": 0, "observed_only": 0, "neither": 0}
    rejected_runs = sum(1 for r in corpus.runs.values() if validate_run(r))
    for pair in corpus.pairs:
        group = corpus.run(pair.get("group_run_id"))
        status = pair_status(group)
        statuses[status] = statuses.get(status, 0) + 1
        decision = policy.decide(pair, corpus, opts)
        observed = (group or {}).get("observed_decision")
        if observed in ("reuse", "refuse"):
            key = ("both_reuse" if decision.skip_all and observed == "reuse" else
                   "replay_only" if decision.skip_all else
                   "observed_only" if observed == "reuse" else "neither")
            replay_vs_observed[key] += 1
        if status == "build_failed" and decision.skip_all:
            build_failures_skipped.append({"pr": pair.get("pr"), "group_run_id": pair.get("group_run_id")})
        if status != "scored":
            continue
        scored += 1
        tests = corpus.tests(group["run_id"])
        if tests is None:
            statuses["scored_without_tests"] = statuses.get("scored_without_tests", 0) + 1
            continue
        group_seconds = sum(float(t.get("duration_s") or 0.0) for t in tests)
        total_seconds += group_seconds
        if not decision.evaluable:
            benefits.append(0.0)
            continue
        evaluable += 1
        if decision.skip_all:
            skipped_groups += 1
        pair_skipped = 0.0
        for t in tests:
            if not decision.skips(t["test_id"]):
                continue
            pair_skipped += float(t.get("duration_s") or 0.0)
            outcome = t.get("outcome")
            attempts = int(t.get("attempts") or 1)
            row = {"pr": pair.get("pr"), "group_run_id": group["run_id"], "test_id": t["test_id"],
                   "outcome": outcome, "attempts": attempts, "reason": decision.reason}
            if outcome in FAIL_OUTCOMES:
                (flake_skips if t.get("exonerated") is True else false_skips).append(row)
            elif outcome == "pass" and attempts > 1:
                flake_skips.append(row)
        skipped_seconds += pair_skipped
        benefits.append(pair_skipped / group_seconds if group_seconds > 0 else 0.0)
    return {
        "policy": policy_name,
        "candidate": policy.candidate,
        "pairs": len(corpus.pairs),
        "statuses": dict(sorted(statuses.items())),
        "scored_pairs": scored,
        "evaluable_pairs": evaluable,
        "coverage": (evaluable / scored) if scored else None,
        "skipped_groups": skipped_groups,
        "benefit_median": statistics.median(benefits) if benefits else None,
        "benefit_p90": _percentile(benefits, 0.9),
        "benefit_pooled": (skipped_seconds / total_seconds) if total_seconds else None,
        "skipped_test_seconds": round(skipped_seconds, 3),
        "group_test_seconds": round(total_seconds, 3),
        "false_skips": len(false_skips),
        "false_skip_rows": false_skips,
        "flake_skips": len(flake_skips),
        "flake_skip_rows": flake_skips,
        "build_failed": statuses.get("build_failed", 0),
        "build_failures_skipped": len(build_failures_skipped),
        "build_failures_skipped_rows": build_failures_skipped,
        "rejected_runs": rejected_runs,
        "replay_vs_observed": replay_vs_observed,
        "verdict": verdict_for(false_skips, scored, evaluable, opts.get("min_sample", 20)),
    }


def verdict_for(false_skips: list, scored: int, evaluable: int, min_sample: int) -> str:
    if false_skips:
        return "UNSAFE: false skips"
    if evaluable < min_sample:
        return "insufficient sample"
    return "safe over the window"


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------

def run_scenarios(directory: Path) -> list[dict]:
    """Score every case of every scenario fixture; report expected vs actual."""
    results = []
    for path in sorted(directory.glob("*.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        if doc.get("schema") != SCENARIO_SCHEMA:
            raise ValueError(f"{path}: schema is not {SCENARIO_SCHEMA}")
        for case in doc["cases"]:
            data = case["corpus"]
            corpus = Corpus(data.get("runs", []), data.get("pairs", []), tests=data.get("tests", {}))
            actual = score(corpus, case["policy"], {"min_sample": 1, **case.get("options", {})})
            mismatches = {k: {"expected": v, "actual": actual.get(k)}
                          for k, v in case["expect"].items() if actual.get(k) != v}
            results.append({"scenario": doc["name"], "case": case["label"], "file": path.name,
                            "policy": case["policy"], "ok": not mismatches, "mismatches": mismatches})
    return results


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _parse_time(value: str) -> dt.datetime:
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


def _fmt(value: Any, pct: bool = False) -> str:
    if value is None:
        return "n/a"
    return f"{100 * value:.1f}%" if pct else str(value)


def render(result: dict) -> str:
    lines = [
        f"policy {result['policy']}{'' if result['candidate'] else ' (control)'}: {result['verdict']}",
        f"  pairs {result['pairs']}  statuses {result['statuses']}",
        f"  scored {result['scored_pairs']}  evaluable {result['evaluable_pairs']}  "
        f"coverage {_fmt(result['coverage'], True)}  groups skipped {result['skipped_groups']}",
        f"  benefit median {_fmt(result['benefit_median'], True)}  p90 {_fmt(result['benefit_p90'], True)}  "
        f"pooled {_fmt(result['benefit_pooled'], True)} "
        f"({result['skipped_test_seconds']:.0f} of {result['group_test_seconds']:.0f} test-seconds)",
        f"  FALSE SKIPS {result['false_skips']}  flake-skips {result['flake_skips']}  "
        f"build_failed {result['build_failed']} (skipped by policy {result['build_failures_skipped']})  "
        f"rejected runs {result['rejected_runs']}",
        f"  replay vs recorded decision {result['replay_vs_observed']}",
    ]
    for row in result["false_skip_rows"][:20]:
        lines.append(f"  false skip: PR {row['pr']} group run {row['group_run_id']}: {row['test_id']} "
                     f"({row['outcome']}, attempts {row['attempts']}; {row['reason']})")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect", help="GitHub history -> neutral corpus")
    c.add_argument("--since", required=True, help="ISO date or datetime (UTC)")
    c.add_argument("--until", default=None, help="ISO date or datetime (UTC); default now")
    c.add_argument("--out", required=True, type=Path)
    c.add_argument("--repository", default="Generous-Corp/pulp")
    c.add_argument("--repo", default=str(REPO_ROOT), type=Path, help="local clone for commit parents and diffs")
    c.add_argument("--token", default=None)
    c.add_argument("--workers", type=int, default=6)
    c.add_argument("--rate-reserve", type=int, default=2500,
                   help="pause when fewer API calls than this remain (the limit is shared)")
    s = sub.add_parser("score", help="score policies over a corpus or the scenario fixtures")
    group = s.add_mutually_exclusive_group(required=True)
    group.add_argument("--corpus", type=Path)
    group.add_argument("--scenarios", type=Path)
    s.add_argument("--policy", action="append", default=None,
                   help=f"one of {', '.join(POLICIES)} (repeatable; default: every implemented candidate)")
    s.add_argument("--receipt-source", choices=("derived", "observed"), default="derived")
    s.add_argument("--since", default=None, help="score only groups created at or after this ISO time")
    s.add_argument("--min-sample", type=int, default=20)
    s.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)

    if a.cmd == "collect":
        since = _parse_time(a.since if "T" in a.since else a.since + "T00:00:00Z")
        until = (_parse_time(a.until if "T" in a.until else a.until + "T23:59:59Z") if a.until
                 else dt.datetime.now(dt.timezone.utc))
        if since.tzinfo is None:
            since = since.replace(tzinfo=dt.timezone.utc)
        import reuse_replay_collect as rrc
        gh = rrc.GitHub(a.repository, a.token, reserve=a.rate_reserve)
        manifest = rrc.Collector(gh, a.out, a.repo, a.workers).collect(since, until)
        print(json.dumps(manifest, indent=2))
        if manifest["merge_groups"] == 0 or manifest["pairs_with_head_run"] == 0:
            print("collect: CONTROL FAILED: no merge groups or no PR-head pairs in the window; "
                  "the instrument is broken, not the history", file=sys.stderr)
            return 1
        return 0

    if a.scenarios:
        results = run_scenarios(a.scenarios)
        if a.json:
            print(json.dumps(results, indent=2))
        else:
            for r in results:
                print(f"{'ok  ' if r['ok'] else 'FAIL'} {r['scenario']}: {r['case']} [{r['policy']}]"
                      + ("" if r["ok"] else f" {r['mismatches']}"))
        return 0 if results and all(r["ok"] for r in results) else 1

    corpus = Corpus.load(a.corpus)
    if a.since:
        cutoff = _parse_time(a.since if "T" in a.since else a.since + "T00:00:00Z")
        corpus.pairs = [p for p in corpus.pairs
                        if (corpus.run(p["group_run_id"]) or {}).get("created_at")
                        and _parse_time(corpus.run(p["group_run_id"])["created_at"]) >= cutoff]
    names = a.policy or [p.name for p in POLICIES.values() if p.candidate and p.decide]
    opts = {"receipt_source": a.receipt_source, "min_sample": a.min_sample}
    results = []
    for name in names:
        try:
            results.append(score(corpus, name, opts))
        except (KeyError, PolicyNotImplemented) as err:
            print(f"score: {err}", file=sys.stderr)
            return 2
    if a.json:
        print(json.dumps(results, indent=2))
    else:
        print("\n\n".join(render(r) for r in results))
    return 1 if any(r["false_skips"] for r in results if POLICIES[r["policy"]].candidate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
