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
    reuse_policy_replay.py source-keys --corpus <dir> --test-map <json> (--graph-pickle P | --build-dir B)

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
  source-key     tier 1a: a test is skipped, and its executable not rebuilt,
                 when its per-executable source key matches the head run's
                 (reconstructed by `source-keys` from git and a build graph)
                 and the head passed it first time. Fails closed on drifted
                 data files, CMake and whole-tree tests; the variant to ship.
  source-key-per-entry
                 the same with script tests keyed on their own entry and
                 compiled tests' data reads assumed declared: the estimate.
  source-key-codemodel
                 source-key, except a CMake change re-keys only executables
                 whose recorded file-API codemodel digest moved between the
                 head and group jobs (or that depend on one). Unevaluable
                 where either job recorded no codemodel.
  source-key-recorded, source-key-codemodel-recorded
                 the same two on the recorded graph: executables, the tests
                 each runs, and the members each link pulled come from the
                 group job's own reuse record, so executable names never go
                 stale; the Ninja graph only maps headers to sources.
  source-key-manifest-data-recorded
                 source-key-codemodel-recorded, except a runtime-surface
                 drift re-runs a compiled test only through its executable's
                 data manifest entry: its declared inputs, any drift for
                 undeclared reads or an unscanned executable, none for a
                 scanned executable that reads nothing.
  output-key-recorded
                 tier 2 estimate: source-key-manifest-data-recorded, except
                 an executable both jobs recorded a hash for is rebuilt only
                 when its bytes differ (early cutoff); the rest keep the
                 source key. It needs the group's build, so it saves test
                 time, not build time.
  source-key-list-level
                 control: a list edit re-runs every declared script test; it
                 must read lower than per-entry.
  suite-source-key, per-executable
                 tier-1b and tier-2 seams: registered, not implemented.
  none, key-match-reference, selection-reference
                 controls for the scenario fixtures: `none` skips nothing;
                 `key-match-reference` skips a test whose head and group
                 records carry the same output_key; `selection-reference`
                 skips what a recorded selection left out. None is a candidate.

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


def contexts_ok(cand: dict, run: dict, mode: str = "red-only") -> bool:
    """Did the head's required contexts allow a receipt, as of the group?

    `strict` (the live verifier): every required context present and green.
    `red-only` (the default for history): none of the contexts present was
    red; a context the head has no check-run for does not refuse, because
    today's required list names contexts that did not exist for most of
    the window. An unreadable record refuses in both modes."""
    if mode == "strict" or cand.get("required_contexts_red") is None:
        green = cand.get("required_contexts_green")
        return (green if green is not None else run.get("required_contexts_green")) is True
    return not cand["required_contexts_red"]


def receipt_eligible(run: dict, source: str = "derived", cand: dict | None = None,
                     contexts: str = "red-only") -> bool:
    """Would this PR-head run have issued a reusable receipt?

    `derived`: it ran the full suite to completion with every test's final
    outcome green, and its required contexts allowed it (`contexts_ok`).
    `observed`: the issuer said it published one."""
    if run.get("run_kind") != "pr_head" or validate_run(run):
        return False
    if source == "observed":
        return run.get("receipt_issued") is True
    ctest = run.get("ctest") or {}
    return bool(ctest.get("complete") and ctest.get("full_suite") is True
                and ctest.get("failed", 1) == 0 and contexts_ok(cand or {}, run, contexts))


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
    # (executables not rebuilt, executables) when the policy also skips builds
    build: tuple[int, int] | None = None
    # (pair fired the spawnable fallback, tests that ran only because of it)
    fallback: tuple[bool, int] | None = None
    # (executables whose recorded bytes changed but the policy did not
    # rebuild, executables compared, executables rebuilt with identical
    # bytes) where both jobs recorded binary hashes
    binaries: tuple[int, int, int] | None = None

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
        if run is not None and receipt_eligible(run, source, cand, opts.get("contexts", "red-only")):
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


def _source_key(variant: str) -> Callable[[dict, Corpus, dict], Decision]:
    """Tier 1a: per-executable source keys (see reuse_replay_collect).

    A test is skipped when the pair's reconstructed key says nothing it is
    built from or reads changed between the head run's tree and the group's,
    AND the head run passed it on the first attempt (a retried pass is not
    evidence). Its executable is then not rebuilt either."""
    def decide(pair: dict, corpus: Corpus, opts: dict) -> Decision:
        group = corpus.run(pair.get("group_run_id"))
        if group is None or validate_run(group):
            return _unevaluable("group checkout unknown or rejected")
        keys = (pair.get("source_key") or {}).get(variant)
        head = corpus.run(pair.get("source_key_head_run_id"))
        if keys is None or head is None or validate_run(head):
            return _unevaluable("no reconstructed source key")
        if not image_compatible(head, group):
            return Decision(True, "runner image differs")
        passed = {t["test_id"] for t in corpus.tests(head["run_id"]) or []
                  if t.get("outcome") == "pass" and int(t.get("attempts") or 1) == 1}
        if not passed:
            return _unevaluable("no head test records")
        must_run = set(keys["run"])
        skip = frozenset(t["test_id"] for t in corpus.tests(group["run_id"]) or []
                         if t["test_id"] in passed and t["test_id"] not in must_run)
        total = int(keys.get("executables_total") or 0)
        build = (total - int(keys.get("executables_rebuilt") or 0), total) if total else None
        binaries = None
        if "unreached_changed_binaries" in keys:
            binaries = (len(keys["unreached_changed_binaries"]), int(keys.get("binaries_compared") or 0),
                        int(keys.get("rebuilt_identical_binaries") or 0))
        fallback = None
        if "spawnable_fallback" in keys:
            fallback = (bool(keys["spawnable_fallback"]), int(keys.get("fallback_only_tests") or 0))
        return Decision(True, f"source key ({variant}): {len(skip)} tests unchanged", skip=skip, build=build,
                        binaries=binaries, fallback=fallback)
    return decide


def decide_selection_reference(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    """Skip every group test outside `pair["selection"]["run"]`, the shape of
    any test-selection rule (an affected-test set, a changed-surface plan).
    No head evidence is consulted: a selection runs what it selects."""
    group = corpus.run(pair.get("group_run_id"))
    if group is None or validate_run(group):
        return _unevaluable("group checkout unknown or rejected")
    selection = pair.get("selection")
    if selection is None:
        return _unevaluable("no recorded selection")
    keep = set(selection.get("run") or [])
    skip = frozenset(t["test_id"] for t in corpus.tests(group["run_id"]) or [] if t["test_id"] not in keep)
    return Decision(True, f"selection skips {len(skip)} tests", skip=skip)


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
    Policy("source-key", _source_key("strict-data"), True,
           "tier 1a: per-executable source key, fail closed on undeclared data and whole-tree tests"),
    Policy("source-key-per-entry", _source_key("per-entry"), True,
           "tier 1a estimate: script tests keyed on their own entry, data reads assumed declared"),
    Policy("source-key-codemodel", _source_key("cmake-codemodel"), True,
           "tier 1a strict with exact CMake granularity: re-key only targets whose recorded codemodel moved"),
    Policy("source-key-recorded", _source_key("strict-data-recorded"), True,
           "tier 1a strict, executables and rebuilds from the group job's recorded link members"),
    Policy("source-key-codemodel-recorded", _source_key("cmake-codemodel-recorded"), True,
           "source-key-codemodel on the recorded graph (link members, recorded test executables)"),
    Policy("source-key-manifest-data-recorded", _source_key("manifest-data-recorded"), True,
           "source-key-codemodel-recorded with the data rule scoped by each executable's data manifest"),
    Policy("output-key-recorded", _source_key("output-key-recorded"), True,
           "tier 2 estimate: source-key-manifest-data-recorded with recorded output hashes deciding rebuilds"),
    Policy("source-key-list-level", _source_key("list-level"), False,
           "control: any change to the script-input list re-runs every declared script test"),
    Policy("suite-source-key", None, True,
           "tier 1b: one source key over the whole suite (not implemented)"),
    Policy("per-executable", None, True,
           "tier 2: output key per test executable (not implemented)"),
    Policy("none", decide_none, False, "control: skips nothing"),
    Policy("selection-reference", decide_selection_reference, False,
           "fixture control: skip what a recorded test selection left out"),
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
    if ctest.get("log_unavailable"):
        return "log_expired"
    if not ctest.get("ran"):
        return "no_suite"  # no native input, a cancelled leg, or the job never reached ctest
    if not ctest.get("complete") and not ctest.get("failed"):
        return "incomplete"  # cut off mid-run with nothing failed yet: no ground truth
    # A run stopped on a failure is ground truth for that failure.
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
    build_fracs: list[float] = []
    unreached: list[dict] = []
    binary_pairs = binaries_compared = rebuilt_identical = 0
    fallback_pairs = fallback_only_pairs = 0
    build_skipped = build_total = 0
    false_skips: list[dict] = []
    flake_skips: list[dict] = []
    build_failures_skipped: list[dict] = []
    evaluable = scored = skipped_groups = failing_tests = 0
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
        failing_tests += sum(1 for t in tests if t.get("outcome") in FAIL_OUTCOMES)
        if decision.skip_all:
            skipped_groups += 1
        if decision.fallback is not None:
            fallback_pairs += int(decision.fallback[0])
            fallback_only_pairs += int(decision.fallback[1] > 0)
        if decision.binaries is not None:
            binary_pairs += 1
            binaries_compared += decision.binaries[1]
            rebuilt_identical += decision.binaries[2]
            if decision.binaries[0]:
                unreached.append({"pr": pair.get("pr"), "group_run_id": group["run_id"], "count": decision.binaries[0]})
        if decision.build is not None:
            build_skipped += decision.build[0]
            build_total += decision.build[1]
            build_fracs.append(decision.build[0] / decision.build[1])
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
        "benefit_p25": _percentile(benefits, 0.25),
        "benefit_p75": _percentile(benefits, 0.75),
        "benefit_p90": _percentile(benefits, 0.9),
        "benefit_pooled": (skipped_seconds / total_seconds) if total_seconds else None,
        "build_skipped_median": statistics.median(build_fracs) if build_fracs else None,
        "build_skipped_pooled": (build_skipped / build_total) if build_total else None,
        "unreached_changed_binaries": sum(u["count"] for u in unreached),
        "unreached_rows": unreached,
        "binary_control_pairs": binary_pairs,
        "binaries_compared": binaries_compared,
        "rebuilt_identical_binaries": rebuilt_identical,
        "spawnable_fallback_pairs": fallback_pairs,
        "fallback_only_rerun_pairs": fallback_only_pairs,
        # The false-skip count can only catch a failure that happened: the
        # number of failing tests the evaluable groups held bounds its power.
        "failing_group_tests": failing_tests,
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
        "verdict": ("UNSAFE: changed binaries not rebuilt" if unreached else
                    verdict_for(false_skips, scored, evaluable, opts.get("min_sample", 20))),
    }


KEY_BLIND_SCHEMA = "pulp-key-blind/v1"
KEY_BLIND_VARIANT = "cmake-codemodel-recorded"


def pairs_since(corpus: Corpus, since: str) -> list[dict]:
    """The corpus's pairs whose merge group was created at or after `since`
    (an ISO date or datetime, UTC). A pair whose group run is unknown is
    dropped: its time cannot be shown to be inside the window."""
    cutoff = _parse_time(since if "T" in since else since + "T00:00:00Z")
    out = []
    for pair in corpus.pairs:
        created = (corpus.run(pair.get("group_run_id")) or {}).get("created_at")
        if created and _parse_time(created) >= cutoff:
            out.append(pair)
    return out


def key_blind_update(pairs: Iterable[dict], existing: dict | None) -> tuple[dict, list[str]]:
    """The key-blind list after one more corpus: every executable whose
    recorded bytes changed between a PR head and its merge group while the
    content-keyed source key did not rebuild it. Pure. The list only grows;
    an entry keeps its `explained` note and loses nothing when it stops
    appearing, because a mechanism that did not fire this week is not a
    mechanism that was keyed. Returns the new list and the names it added."""
    doc = {"schema": KEY_BLIND_SCHEMA, "executables": {}}
    if existing:
        if existing.get("schema") != KEY_BLIND_SCHEMA:
            raise ValueError(f"key-blind list schema is not {KEY_BLIND_SCHEMA}")
        doc["executables"] = {k: dict(v) for k, v in (existing.get("executables") or {}).items()}
    added: list[str] = []
    for pair in pairs:
        keys = pair.get("source_key") or {}
        if keys.get("content_keyed") is not True:
            continue
        for exe in (keys.get(KEY_BLIND_VARIANT) or {}).get("unreached_changed_binaries") or []:
            entry = doc["executables"].get(exe)
            if entry is None:
                entry = doc["executables"][exe] = {"example": {"pr": pair.get("pr"),
                                                                  "group_run_id": pair.get("group_run_id")},
                                                   "explained": None}
                added.append(exe)
    doc["executables"] = dict(sorted(doc["executables"].items()))
    return doc, sorted(added)


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
        f"  benefit median {_fmt(result['benefit_median'], True)} (p25 {_fmt(result['benefit_p25'], True)}, "
        f"p75 {_fmt(result['benefit_p75'], True)})  p90 {_fmt(result['benefit_p90'], True)}  "
        f"pooled {_fmt(result['benefit_pooled'], True)} "
        f"({result['skipped_test_seconds']:.0f} of {result['group_test_seconds']:.0f} test-seconds)",
        f"  power: the evaluable groups held {result['failing_group_tests']} failing tests, so the false-skip "
        f"count below can catch at most that many; the binary control cannot see data reads",
        f"  build skipped (executables not rebuilt) median {_fmt(result['build_skipped_median'], True)} "
        f"pooled {_fmt(result['build_skipped_pooled'], True)}",
        f"  binary control: {result['unreached_changed_binaries']} changed binaries not rebuilt "
        f"({result['binaries_compared']} compared over {result['binary_control_pairs']} pairs); "
        f"{result['rebuilt_identical_binaries']} rebuilt with identical bytes",
        f"  spawnable fallback fired in {result['spawnable_fallback_pairs']} pairs; the only reason tests "
        f"ran in {result['fallback_only_rerun_pairs']} (0 = free; above 0 = the source-scan guard is due)",
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
    k = sub.add_parser("source-keys", help="reconstruct tier-1a source keys into a corpus's pairs")
    k.add_argument("--corpus", required=True, type=Path)
    k.add_argument("--test-map", required=True, type=Path,
                   help="JSON {tests: {name: {executables: [...], sources: [...]}}} for the graph's build dir")
    k.add_argument("--build-dir", type=Path, help="configured Ninja build dir the graph and map describe")
    k.add_argument("--graph-pickle", type=Path, help="a pickled affected_tests_shadow Graph (faster)")
    k.add_argument("--source-root", type=Path, help="checkout the graph's paths are under (default: the build dir's parent)")
    k.add_argument("--repo", default=str(REPO_ROOT), type=Path)
    k.add_argument("--runs", type=Path, help="JSON list of group runs ([{run: id}] or ids) to restrict to")
    k.add_argument("--codemodel", action="store_true",
                   help="also write the cmake-codemodel variant from each job's recorded reuse-record codemodel")
    k.add_argument("--legacy-propagation", action="store_true",
                   help="propagate a codemodel change to every dependent, link or run-time edge alike")
    k.add_argument("--legacy-rules", action="store_true",
                   help="keep the root-CMakeLists and learned commit-bound rules even for content-keyed (v2) records")
    k.add_argument("--repository", default="Generous-Corp/pulp")
    k.add_argument("--token", default=None)
    b = sub.add_parser("key-blind", help="grow the key-blind list from a corpus's content-keyed pairs")
    b.add_argument("--corpus", required=True, type=Path)
    b.add_argument("--list", type=Path, default=REPO_ROOT / "tools" / "ci" / "key_blind_executables.json")
    b.add_argument("--write", action="store_true", help="write the grown list (default: report only)")
    b.add_argument("--since", default=None,
                   help="only groups created at or after this ISO time: start after the change that "
                        "made a delisted executable deterministic, or its older misses list it again")
    s = sub.add_parser("score", help="score policies over a corpus or the scenario fixtures")
    group = s.add_mutually_exclusive_group(required=True)
    group.add_argument("--corpus", type=Path)
    group.add_argument("--scenarios", type=Path)
    s.add_argument("--policy", action="append", default=None,
                   help=f"one of {', '.join(POLICIES)} (repeatable; default: every implemented candidate)")
    s.add_argument("--receipt-source", choices=("derived", "observed"), default="derived")
    s.add_argument("--contexts", choices=("red-only", "strict"), default="red-only",
                   help="derived receipts: refuse on a red required context only (history), or also on an absent one (the live rule)")
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
        try:
            manifest = rrc.Collector(gh, a.out, a.repo, a.workers).collect(since, until)
        except rrc.ListingShort as err:
            print(f"collect: LISTING SHORT: {err}; the corpus is not rewritten: every pair needing a missing run "
                  "would have been dropped", file=sys.stderr)
            return 1
        print(json.dumps(manifest, indent=2))
        cov = manifest.get("record_coverage") or {}
        print(f"collect: record coverage: {cov.get('without_record')} of {cov.get('executed_jobs')} executed macOS jobs "
              f"since {cov.get('since')} published no reuse record (expected 0); "
              f"{cov.get('interrupted_jobs')} jobs lost their runner before the record step", file=sys.stderr)
        if manifest["merge_groups"] == 0 or manifest["pairs_with_head_run"] == 0:
            print("collect: CONTROL FAILED: no merge groups or no PR-head pairs in the window; "
                  "the instrument is broken, not the history", file=sys.stderr)
            return 1
        return 0

    if a.cmd == "source-keys":
        import reuse_replay_collect as rrc
        doc = json.loads(a.test_map.read_text(encoding="utf-8"))
        build_dir = a.build_dir or Path(doc["build_dir"])
        if a.graph_pickle is None and a.build_dir is None:
            ap.error("--graph-pickle or --build-dir is required")
        graph = rrc.load_graph(a.build_dir, a.graph_pickle)
        only = None
        if a.runs:
            only = {str(x["run"] if isinstance(x, dict) else x) for x in json.loads(a.runs.read_text())}
        gh = rrc.GitHub(a.repository, a.token) if a.codemodel else None
        result = rrc.annotate_source_keys(a.corpus, a.repo, graph, a.source_root or build_dir.parent,
                                          build_dir, doc["tests"], only, gh, a.legacy_rules,
                                          a.legacy_propagation)
        print(json.dumps(result))
        return 0 if result["pairs_annotated"] else 1

    if a.cmd == "key-blind":
        existing = json.loads(a.list.read_text(encoding="utf-8")) if a.list.is_file() else None
        corpus = Corpus.load(a.corpus)
        doc, added = key_blind_update(pairs_since(corpus, a.since) if a.since else corpus.pairs, existing)
        for exe in added:
            print(f"key-blind: NEW {exe}")
        print(f"key-blind: {len(doc['executables'])} listed, {len(added)} new")
        if a.write:
            a.list.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
        return 1 if added and not a.write else 0

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
        corpus.pairs = pairs_since(corpus, a.since)
    names = a.policy or [p.name for p in POLICIES.values() if p.candidate and p.decide]
    opts = {"receipt_source": a.receipt_source, "contexts": a.contexts, "min_sample": a.min_sample}
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
