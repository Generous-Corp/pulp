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
                 script test (`test/ctest_script_inputs/` at the group's
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
  entry-delta-docs
                 build-skipping: a merge group skips its native build when its
                 OWN entry's delta (the group commit against its first parent,
                 `first_parent_delta_files`) is `docs-only` by
                 `classify_changes.impact_class`, stacked or not; any other
                 class, or an unreadable delta, builds. It reads no PR-head run,
                 so `collect --groups-only` is enough. It is scored under the
                 ALLGREEN attribution rule: a skipped group whose build or
                 tests failed is a false skip only when its predecessor group's
                 `macos` passed, its first parent is main's tip, or the
                 predecessor cannot be found (fail closed); a failure behind a
                 red or cancelled predecessor is a `neighbour_failure`, counted
                 separately, because under ALLGREEN the predecessor's entry
                 does not land and this group is rebuilt without it.
                 Each remaining failure is then attributed per failing test,
                 failing closed at every unread fact, in this order:
                 `docs_caused` (the test is in the deterministic tier, its
                 declared inputs meet the delta or are missing, its
                 executable or sources are unknown or name a delta path, or a
                 delta path is a FORCE_BUILD_PREFIXES input or named by
                 non-test CMake); `exonerated` (replay-only hindsight, never
                 a live verdict: C, the same head later landed through a
                 green group and the first main push run containing it was
                 green; or B, the test failed in >= 2 other PRs' non-docs
                 groups of other batches within 7 days and passed later in a
                 landed group); else `unexplained`. The verdict needs
                 docs_caused == 0, unexplained == 0, and exonerated groups /
                 skipped native groups at or below the window's merge-group
                 `macos` red rate; the raw false-skip count is still printed.
  suite-source-key, per-executable
                 tier-1b and tier-2 seams: registered, not implemented.
  none, key-match-reference, selection-reference
                 controls for the scenario fixtures: `none` skips nothing;
                 `key-match-reference` skips a test whose head and group
                 records carry the same output_key; `selection-reference`
                 skips what a recorded selection left out. None is a candidate.

METRICS (entry-delta policies, besides the above)

  neighbour_failures  failures in skipped groups that the ALLGREEN rule
               attributes to a red predecessor group; reported, not false.
  unattributed_failures  skipped groups whose `macos` failed with no build or
               test failure on record (an infrastructure failure, an expired
               log); reported beside the zero so a reader can check them.
  skip_all_groups / skip_all_native_groups / skip_all_macos_minutes
               over every pair, whatever its status: the groups the policy
               would skip, those of them whose own `macos` ran the native gate
               (over one minute), and the `macos` minutes those groups spent.

Exit codes: 0 success; 1 `score` found a false skip or a scenario verdict
mismatch, or `collect` found no merge groups or no PR-head pairs (the
controls); 2 invalid input.
"""
from __future__ import annotations

import argparse
import collections
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
SCRIPT_INPUTS_PATH = "test/ctest_script_inputs"


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

    def group_by_sha(self, sha: Any) -> dict | None:
        """The merge-group run whose commit is `sha` (the latest created)."""
        if not sha:
            return None
        found = [r for r in self.runs.values() if r.get("run_kind") == "merge_group" and r.get("group_sha") == sha]
        return max(found, key=lambda r: str(r.get("created_at") or "")) if found else None


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
    # "allgreen": a failure in a skipped group is attributed to its
    # predecessor group first (see `allgreen_blame`).
    attribution: str = "none"


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


def decide_entry_delta_docs(pair: dict, corpus: Corpus, opts: dict) -> Decision:
    """Skip the build when the group's own entry delta is docs-only.

    The delta is the group commit against its first parent, so for a group
    stacked on entries ahead it is the last entry alone: the design leans on
    ALLGREEN, where every group ahead had to pass on its own. Fail closed: an
    unreadable delta runs everything."""
    group = corpus.run(pair.get("group_run_id"))
    if group is None or group.get("run_kind") != "merge_group" or validate_run(group):
        return _unevaluable("group checkout unknown or rejected")
    files = group.get("first_parent_delta_files")
    if files is None:
        return _unevaluable("entry delta unreadable")
    change_class = classify_changes.impact_class(list(files))
    if change_class == classify_changes.DOCS_ONLY:
        return Decision(True, f"entry delta docs-only ({len(files)} files)", skip_all=True)
    return Decision(True, f"entry delta {change_class}")


EXONERATION_WINDOW = dt.timedelta(days=7)
CORROBORATING_PRS = 2
QUARANTINE_CANDIDATE_GROUPS = 3


def _delta_hits(paths: list[str], delta: list[str]) -> bool:
    """A declared input equals a delta path or is a directory holding one."""
    return any(d == p or d.startswith(p.rstrip("/") + "/") for p in paths for d in delta)


def docs_caused(group: dict, test_id: str) -> str | None:
    """Why the docs-only delta may have caused this failure, or None.

    Fails closed: every unread fact is a reason."""
    delta = list(group.get("first_parent_delta_files") or [])
    if not delta:
        return "entry delta unreadable"
    if any(d.startswith(classify_changes.FORCE_BUILD_PREFIXES) for d in delta):
        return "a delta path is a FORCE_BUILD_PREFIXES input"
    named = group.get("delta_cmake_named")
    if named is None:
        return "non-test CMake not scanned"
    if named:
        return f"non-test CMake names a delta path ({named[0]})"
    if test_id == "(build)":
        return None
    facts = (group.get("entry_test_facts") or {}).get(test_id)
    if facts is None:
        return "no facts recorded for the test"
    if facts.get("deterministic") is not False:
        return "deterministic tier" if facts.get("deterministic") else "tier unknown"
    inputs = facts.get("declared_inputs")
    if facts.get("kind") == "script":
        if inputs is None:
            return "script test without a declared-inputs record"
        return "declared inputs include the delta" if _delta_hits(inputs, delta) else None
    if facts.get("kind") == "compiled":
        if not facts.get("executable"):
            return "executable unknown"
        if inputs and _delta_hits(inputs, delta):
            return "declared data inputs include the delta"
        naming = facts.get("sources_naming_delta")
        if naming is None:
            return "executable sources unknown"
        return f"a source names a delta path ({naming[0]})" if naming else None
    return "test kind unknown"


def landed_green(group: dict, corpus: Corpus) -> dict | None:
    """C: the same head later landed through a green group, and the first
    main push run containing the merge was green. Evidence, or None."""
    merge = group.get("pr_merge") or {}
    if not merge.get("merged_at") or merge.get("head_sha") != group.get("head_sha"):
        return None
    landing = corpus.run(group.get("landing_group_run_id"))
    push = group.get("main_push_run") or {}
    if (landing is None or landing.get("group_sha") != merge.get("merge_commit_sha")
            or landing.get("macos_conclusion") != "success" or push.get("conclusion") != "success"):
        return None
    return {"rule": "C", "landing_group_run_id": landing["run_id"], "main_push_run_id": push.get("run_id")}


def _corroborated(group: dict, test_id: str, outcomes: dict[str, list[tuple[dict, str]]]) -> dict | None:
    """B: the test failed in >= 2 other PRs' groups within the window whose
    entry delta was not docs-only and whose batch sits on another main
    commit, and it passed in a later group that landed. Evidence, or None."""
    created = _parse_time(group["created_at"])
    batch = group.get("main_base")
    if not batch:
        return None
    others: dict[int, str] = {}
    passed_later = None
    for run, outcome in outcomes.get(test_id, []):
        when = _parse_time(run["created_at"])
        if outcome == "pass" and when > created and run.get("landed") is True and passed_later is None:
            passed_later = run["run_id"]
        if outcome not in FAIL_OUTCOMES or run is group or run.get("pr") in (None, group.get("pr")):
            continue
        if abs(when - created) > EXONERATION_WINDOW:
            continue
        delta = run.get("first_parent_delta_files")
        if not delta or classify_changes.impact_class(list(delta)) == classify_changes.DOCS_ONLY:
            continue
        if not run.get("main_base") or run["main_base"] == batch:
            continue
        others.setdefault(run["pr"], run["run_id"])
    if len(others) < CORROBORATING_PRS or passed_later is None:
        return None
    return {"rule": "B", "corroborating": [{"pr": pr, "group_run_id": rid} for pr, rid in sorted(others.items())],
            "passed_later_group_run_id": passed_later}


def attribute_failures(corpus: Corpus, rows: list[dict], skipped_native_groups: int) -> dict:
    """Attribute each non-neighbour failure in a skipped group (see
    POLICIES["entry-delta-docs"]): docs_caused, then exonerated, else
    unexplained. Pure over the corpus."""
    needed = {r["test_id"] for r in rows}
    outcomes: dict[str, list[tuple[dict, str]]] = collections.defaultdict(list)
    if needed:
        for run in corpus.runs.values():
            if run.get("run_kind") != "merge_group":
                continue
            for t in corpus.tests(run["run_id"]) or []:
                if t["test_id"] in needed:
                    outcomes[t["test_id"]].append((run, t.get("outcome")))
    caused, exonerated, unexplained = [], [], []
    for row in rows:
        group = corpus.run(row["group_run_id"]) or {}
        reason = docs_caused(group, row["test_id"])
        if reason:
            caused.append(dict(row, attribution=reason))
            continue
        evidence = landed_green(group, corpus)
        if evidence is None and row["test_id"] != "(build)":
            evidence = _corroborated(group, row["test_id"], outcomes)
        (exonerated if evidence else unexplained).append(dict(row, evidence=evidence))
    groups = {str(r["group_run_id"]) for r in exonerated}
    per_test = collections.Counter(r["test_id"] for r in {(r["test_id"], str(r["group_run_id"])): r
                                                           for r in exonerated}.values())
    merge_groups = [r for r in corpus.runs.values() if r.get("run_kind") == "merge_group"]
    red = sum(1 for r in merge_groups if r.get("macos_conclusion") == "failure")
    return {
        "docs_caused": len(caused), "docs_caused_rows": caused,
        "exonerated": len(exonerated), "exonerated_rows": exonerated, "exonerated_groups": len(groups),
        "unexplained": len(unexplained), "unexplained_rows": unexplained,
        "exoneration_rate": (len(groups) / skipped_native_groups) if skipped_native_groups else 0.0,
        "macos_red_rate": (red / len(merge_groups)) if merge_groups else None,
        "quarantine_candidates": sorted(t for t, n in per_test.items() if n >= QUARANTINE_CANDIDATE_GROUPS),
    }


def attributed_verdict(result: dict, min_sample: int) -> str:
    if result["docs_caused"]:
        return "UNSAFE: docs-caused failures"
    if result["unexplained"]:
        return "UNSAFE: unexplained failures"
    red = result["macos_red_rate"]
    if red is None or result["exoneration_rate"] > red:
        return "UNSAFE: exonerated groups above the window's macos red rate"
    if result["evaluable_pairs"] < min_sample:
        return "insufficient sample"
    return "safe over the window"


def allgreen_blame(pair: dict, group: dict, corpus: Corpus) -> str:
    """Who a failure in a skipped group belongs to under ALLGREEN.

    `false_skip` when the first parent is main's tip (the pair is not stacked),
    when the predecessor group's `macos` passed, or when the predecessor cannot
    be found or read (fail closed); `neighbour_failure` only when the
    predecessor group's `macos` was red, cancelled or timed out: that entry
    does not land, and GitHub rebuilds this group without it."""
    if pair.get("stacked") is False:
        return "false_skip"
    predecessor = corpus.group_by_sha(group.get("base_sha"))
    if predecessor is None:
        return "false_skip"
    if predecessor.get("macos_conclusion") in ("failure", "cancelled", "timed_out"):
        return "neighbour_failure"
    return "false_skip"


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
    Policy("entry-delta-docs", decide_entry_delta_docs, True,
           "build-skipping: skip the native build when the entry's own delta is docs-only (ALLGREEN attribution)",
           attribution="allgreen"),
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
    neighbour_failures: list[dict] = []
    unattributed_failures: list[dict] = []
    skip_all_groups = skip_all_native_groups = 0
    skip_all_macos_minutes = 0.0
    allgreen = policy.attribution == "allgreen"
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
        if decision.skip_all:
            skip_all_groups += 1
            minutes = (group or {}).get("macos_minutes")
            if isinstance(minutes, (int, float)):
                skip_all_macos_minutes += float(minutes)
                skip_all_native_groups += int(minutes > 1.0)
        blame = allgreen_blame(pair, group, corpus) if allgreen and decision.skip_all and group else "false_skip"
        if status == "build_failed" and decision.skip_all:
            build_failures_skipped.append({"pr": pair.get("pr"), "group_run_id": pair.get("group_run_id")})
            if allgreen:
                row = {"pr": pair.get("pr"), "group_run_id": pair.get("group_run_id"), "test_id": "(build)",
                       "outcome": "build_failed", "attempts": 1, "reason": decision.reason}
                (neighbour_failures if blame == "neighbour_failure" else false_skips).append(row)
        if (allgreen and decision.skip_all and status not in ("scored", "build_failed")
                and (group or {}).get("macos_conclusion") == "failure"):
            unattributed_failures.append({"pr": pair.get("pr"), "group_run_id": pair.get("group_run_id"),
                                          "status": status})
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
                if t.get("exonerated") is True:
                    flake_skips.append(row)
                elif blame == "neighbour_failure":
                    neighbour_failures.append(row)
                else:
                    false_skips.append(row)
            elif outcome == "pass" and attempts > 1:
                flake_skips.append(row)
        skipped_seconds += pair_skipped
        benefits.append(pair_skipped / group_seconds if group_seconds > 0 else 0.0)
    result = {
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
        "neighbour_failures": len(neighbour_failures),
        "neighbour_failure_rows": neighbour_failures,
        "unattributed_failures": len(unattributed_failures),
        "unattributed_failure_rows": unattributed_failures,
        "skip_all_groups": skip_all_groups,
        "skip_all_native_groups": skip_all_native_groups,
        "skip_all_macos_minutes": round(skip_all_macos_minutes, 2),
        "build_failed": statuses.get("build_failed", 0),
        "build_failures_skipped": len(build_failures_skipped),
        "build_failures_skipped_rows": build_failures_skipped,
        "rejected_runs": rejected_runs,
        "replay_vs_observed": replay_vs_observed,
        "verdict": ("UNSAFE: changed binaries not rebuilt" if unreached else
                    verdict_for(false_skips, scored, evaluable, opts.get("min_sample", 20))),
    }
    if allgreen:
        result.update(attribute_failures(corpus, false_skips, skip_all_native_groups))
        result["verdict"] = attributed_verdict(result, opts.get("min_sample", 20))
    return result


KEY_BLIND_SCHEMA = "pulp-key-blind/v1"
STAGE0_POLICY = "source-key-codemodel-recorded"
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


class CutoffUnproven(ValueError):
    """The window a commit opens cannot be shown from the corpus."""


def cutoff_after(corpus: Corpus, commit: str, contains: Callable[[str, str], bool | None]) -> dict:
    """The `--since` a fix opens, derived rather than chosen: the created_at
    of the first merge group whose checkout contains `commit`. The window is
    only a time if time and ancestry agree, so every group created at or
    after the cutoff must contain the commit too; `contains(commit,
    checkout)` answers True, False, or None when it cannot tell, and any
    None or later group without the commit raises instead of guessing."""
    groups = sorted((r for r in corpus.runs.values() if r.get("run_kind") == "merge_group"
                     and r.get("checkout_sha") and r.get("created_at")), key=lambda r: r["created_at"])
    answers = [(g, contains(commit, g["checkout_sha"])) for g in groups]
    unknown = [g["run_id"] for g, v in answers if v is None]
    if unknown:
        raise CutoffUnproven(f"cannot tell whether {len(unknown)} group checkouts contain {commit}: "
                             f"{', '.join(map(str, unknown[:10]))}")
    first = next((i for i, (_, v) in enumerate(answers) if v), None)
    if first is None:
        raise CutoffUnproven(f"no merge group in the corpus contains {commit}")
    without = [g["run_id"] for g, v in answers[first:] if not v]
    if without:
        raise CutoffUnproven(f"{len(without)} groups created after the first that contains {commit} do not "
                             f"contain it, so no time window matches: {', '.join(map(str, without[:10]))}")
    g = answers[first][0]
    return {"since": g["created_at"], "group_run_id": g["run_id"], "pr": g.get("pr"),
            "groups_after": len(answers) - first}


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
        f"  would skip {result['skip_all_groups']} of {result['pairs']} groups (any status); "
        f"{result['skip_all_native_groups']} of them ran the native macos gate, "
        f"{result['skip_all_macos_minutes']:.1f} macos minutes",
    ]
    if POLICIES.get(result["policy"]) and POLICIES[result["policy"]].attribution == "allgreen":
        lines.append(f"  ALLGREEN attribution: neighbour failures {result['neighbour_failures']} "
                     f"(behind a red predecessor group; not false skips)  unattributed macos failures "
                     f"{result['unattributed_failures']}")
        for row in result["neighbour_failure_rows"][:20]:
            lines.append(f"  neighbour failure: PR {row['pr']} group run {row['group_run_id']}: {row['test_id']}")
        for row in result["unattributed_failure_rows"][:20]:
            lines.append(f"  unattributed: PR {row['pr']} group run {row['group_run_id']} ({row['status']})")
        lines.append(f"  attributed (raw FALSE SKIPS above): docs_caused {result['docs_caused']}  "
                     f"unexplained {result['unexplained']}  exonerated {result['exonerated']} "
                     f"({result['exonerated_groups']} groups; replay-only)  exonerated groups / skipped native "
                     f"groups {_fmt(result['exoneration_rate'], True)} vs window macos red rate "
                     f"{_fmt(result['macos_red_rate'], True)}")
        for row in result["docs_caused_rows"]:
            lines.append(f"  docs_caused: PR {row['pr']} group run {row['group_run_id']}: {row['test_id']} "
                         f"({row['attribution']})")
        for row in result["unexplained_rows"]:
            lines.append(f"  unexplained: PR {row['pr']} group run {row['group_run_id']}: {row['test_id']}")
        for row in result["exonerated_rows"]:
            ev = row["evidence"]
            if ev["rule"] == "C":
                why = f"C: landed by group run {ev['landing_group_run_id']}, main push run {ev['main_push_run_id']}"
            else:
                why = ("B: failed in " + ", ".join(f"PR {c['pr']} run {c['group_run_id']}" for c in ev["corroborating"])
                       + f"; passed later in landed group run {ev['passed_later_group_run_id']}")
            lines.append(f"  exonerated: PR {row['pr']} group run {row['group_run_id']}: {row['test_id']} ({why})")
        if result["quarantine_candidates"]:
            lines.append("  quarantine candidates (exonerated in >= 3 docs groups): "
                         + ", ".join(result["quarantine_candidates"]))
    for row in result["false_skip_rows"][:20]:
        lines.append(f"  false skip: PR {row['pr']} group run {row['group_run_id']}: {row['test_id']} "
                     f"({row['outcome']}, attempts {row['attempts']}; {row['reason']})")
    if "same_bytes_rows" in result:
        lines.append(f"  same-bytes nondeterministic {result['same_bytes_nondeterministic']} of "
                     f"{result['false_skips']} false skips (net {result['false_skips_net']})")
        for row in result["same_bytes_rows"][:20]:
            lines.append(f"  same-bytes: group run {row['group_run_id']} {row['test_id']}: head {row['head_run_id']} "
                         f"{(row['head_digest'] or '-')[:16]} group {(row['group_digest'] or '-')[:16]} "
                         f"witness {row['witness_run_id'] or 'none'}")
    return "\n".join(lines)


def stage0_main(a) -> int:
    """Exit 2 when a group's ancestry cannot be told or the restart pair is
    not in the corpus; 1 on a watched-executable regression or a blind data
    scan read as scanned; else 0."""
    import reuse_replay_collect as rrc
    corpus = Corpus.load(a.corpus)
    gh = rrc.GitHub(a.repository, None)
    st = rrc.stage0_pairs(a.corpus, corpus, a.fix, a.watch, rrc.ancestry(a.repo, a.repository, gh), a.repo, gh)
    rows = st["pairs"]
    fs = score(corpus, STAGE0_POLICY)["false_skip_rows"]
    run = rrc.trailing_clean(rows, a.restart_run, {str(r["group_run_id"]) for r in fs})
    dc = rrc.data_scan_control(a.corpus, corpus, a.repo, gh)
    # The same control over the lists the (b) window's pairs read, so the
    # window's exit stands without the older lists.
    window_ids = {str(r["group_run_id"]) for r in rows}
    window = Corpus(list(corpus.runs.values()), [p for p in corpus.pairs if str(p.get("group_run_id")) in window_ids])
    dc_window = rrc.data_scan_control(a.corpus, window, a.repo, gh)
    regressions = [(r["pr"], r["group_run_id"], r["regression"]) for r in rows if r["regression"]]
    if a.json:
        print(json.dumps({"pairs": rows, "ancestry_unknown": st["ancestry_unknown"], "trailing_clean": run,
                          "false_skips": fs, "data_scan_control": dc,
                          "data_scan_control_in_window": dc_window}, indent=1))
    else:
        kb = [r["key_blind"] for r in rows]
        print(f"stage0: {len(rows)} content-keyed pairs whose group contains {a.fix}; "
              f"ancestry unknown {len(st['ancestry_unknown'])}")
        print(f"stage0: (b) trailing clean run {run['count']} from restart run {a.restart_run}"
              + (f", broken by PR #{run['broken_by']['pr']} group {run['broken_by']['group_run_id']}"
                 if run["broken_by"] else "") + ("" if run["restart_found"] else " (restart run NOT in the corpus)")
              + f"; false skips on {STAGE0_POLICY}: {len(fs)}")
        print(f"stage0: watched {len(a.watch)}: commit_bound-absorbed in {sum(1 for r in rows if r['commit_bound'])} pairs, "
              f"key_blind-absorbed {dict(sorted(collections.Counter(kb).items(), key=str))}, "
              f"relinked-clean {sum(1 for r in rows if r['relinked_clean'])}, "
              f"rebuilt in {sum(1 for r in rows if r['rebuilt'])}, REGRESSION {len(regressions)} {regressions[:10]}")
        print(f"stage0: unreached outside the watched executables: "
              f"{[(r['pr'], r['group_run_id'], r['unreached_outside']) for r in rows if r['unreached_outside']][:10]}")
        print(f"stage0: (c) detected-reader control: {dc['lists']} lists, {dc['declaring']} declare a data scan, "
              f"{dc['holds']} hold, fails {dc['fails']}, no data scan {dc['no_data_scan']}, unreadable {dc['unreadable']}; "
              f"negative control: {dc['pairs_on_failing_list']} pairs on a failing list, "
              f"{dc['read_as_scanned_on_failing_list']} read as scanned (must be 0)")
        print(f"stage0: (c) within the (b) window: {dc_window['lists']} lists at the head and group checkouts of "
              f"{len(rows)} pairs, {dc_window['declaring']} declare a data scan, {dc_window['holds']} hold, "
              f"fails {dc_window['fails']}, no data scan {dc_window['no_data_scan']}, "
              f"unreadable {dc_window['unreadable']}")
    if st["ancestry_unknown"] or not run["restart_found"]:
        return 2
    return 1 if regressions or dc["read_as_scanned_on_failing_list"] else 0


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
    c.add_argument("--groups-only", action="store_true",
                   help="record merge groups only, no PR-head runs (enough for entry-delta-docs)")
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
    b.add_argument("--since-commit", default=None,
                   help="derive --since from a fix commit: the created_at of the first merge group whose "
                        "checkout contains it (every later group must contain it too)")
    b.add_argument("--repo", default=str(REPO_ROOT), type=Path, help="local clone for ancestry")
    b.add_argument("--repository", default="Generous-Corp/pulp")
    b.add_argument("--since", default=None,
                   help="only groups created at or after this ISO time: start after the change that "
                        "made a delisted executable deterministic, or its older misses list it again")
    z = sub.add_parser("stage0", help="the Stage 0 read-back: trailing clean run, watched executables, data-scan control")
    z.add_argument("--corpus", required=True, type=Path)
    z.add_argument("--fix", required=True, help="commit whose groups are scored (the restart pair's fix)")
    z.add_argument("--restart-run", default=None, help="group run the trailing clean run counts from")
    z.add_argument("--watch", action="append", default=[], help="executable basename the fix made deterministic")
    z.add_argument("--repo", default=str(REPO_ROOT), type=Path)
    z.add_argument("--repository", default="Generous-Corp/pulp")
    z.add_argument("--json", action="store_true")
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
    s.add_argument("--same-bytes", action="store_true",
                   help="also report which false skips are same-bytes nondeterministic (identical executable "
                        "bytes in head and group, and a third run that passed the test on them); the exit "
                        "status still counts every false skip")
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
            manifest = rrc.Collector(gh, a.out, a.repo, a.workers).collect(since, until,
                                                                         groups_only=a.groups_only)
        except rrc.ListingShort as err:
            print(f"collect: LISTING SHORT: {err}; the corpus is not rewritten: every pair needing a missing run "
                  "would have been dropped", file=sys.stderr)
            return 1
        print(json.dumps(manifest, indent=2))
        cov = manifest.get("record_coverage") or {}
        print(f"collect: record coverage: {cov.get('without_record')} of {cov.get('executed_jobs')} executed macOS jobs "
              f"since {cov.get('since')} published no reuse record (expected 0); "
              f"{cov.get('interrupted_jobs')} jobs lost their runner before the record step", file=sys.stderr)
        for row in manifest.get("listing_shortfalls") or []:
            print(f"collect: instrument event: {row['event']} runs created {row['day']} listed {row['listed']} of "
                  f"total_count {row['total_count']}; whole when asked again", file=sys.stderr)
        if manifest["merge_groups"] == 0 or (manifest["pairs_with_head_run"] == 0 and not a.groups_only):
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
        since = a.since
        if a.since_commit:
            if a.since:
                print("key-blind: --since and --since-commit are exclusive", file=sys.stderr)
                return 2
            import reuse_replay_collect as rrc
            try:
                cut = cutoff_after(corpus, a.since_commit, rrc.ancestry(a.repo, a.repository))
            except CutoffUnproven as err:
                print(f"key-blind: CUTOFF UNPROVEN: {err}", file=sys.stderr)
                return 2
            since = cut["since"]
            print(f"key-blind: since {since}: first group containing {a.since_commit} is run "
                  f"{cut['group_run_id']} (PR #{cut['pr']}); {cut['groups_after']} groups from there all contain it")
        doc, added = key_blind_update(pairs_since(corpus, since) if since else corpus.pairs, existing)
        for exe in added:
            print(f"key-blind: NEW {exe}")
        print(f"key-blind: {len(doc['executables'])} listed, {len(added)} new")
        if a.write:
            a.list.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
        return 1 if added and not a.write else 0

    if a.cmd == "stage0":
        return stage0_main(a)

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
    if a.same_bytes:
        import reuse_replay_collect as rrc
        for r in results:
            rows = rrc.same_bytes_evidence(a.corpus, corpus, r["false_skip_rows"])
            r["same_bytes_rows"] = rows
            r["same_bytes_nondeterministic"] = sum(1 for x in rows if x["qualifies"])
            r["false_skips_net"] = r["false_skips"] - r["same_bytes_nondeterministic"]
    if a.json:
        print(json.dumps(results, indent=2))
    else:
        print("\n\n".join(render(r) for r in results))
    def unsafe(r: dict) -> bool:
        if POLICIES[r["policy"]].attribution == "allgreen":
            return r["verdict"].startswith("UNSAFE")
        return bool(r["false_skips"])
    return 1 if any(unsafe(r) for r in results if POLICIES[r["policy"]].candidate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
