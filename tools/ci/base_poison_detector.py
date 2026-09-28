#!/usr/bin/env python3
"""Decide whether `main` itself is carrying the failure a merge queue keeps ejecting batches for.

WHAT THIS EXISTS TO STOP. A merge_group batch is `main` plus the queued entries.
When main carries a failing test, every batch inherits it, fails after a full
gate, ejects its innocent entries, re-forms, and pays again. Nothing in the
batch's own report says the base is the cause, so each batch is read as a new
culprit in turn.

WHY THE LANE THAT WAS SUPPOSED TO CATCH THIS NEVER REPORTED. `push` to main was
designated the detector: the one lane that runs the whole macOS suite on a
commit actually on main. It produced no observation at all. Every non-proof
event shares the `build-<github.ref>` concurrency group, so every push to main
lands in one domain with `cancel-in-progress` false. GitHub holds at most ONE
run pending per group and cancels the previously pending one when another
arrives, so on a main that merges faster than the suite takes, the intended
"serialize to one leg at a time" degrades into "cancel all but the one already
running" -- and that one's self-hosted macOS leg is still waiting for a runner
when the next merge cancels it too. Measured over the 60 most recent pushes to
main: 58 completed, 0 executed the macOS suite, 55 dispatched no job at all.
The same query over merge_group returned 31 of 55 executed, so the instrument
reads a real lane when there is one to read.

The consequence is not a weaker signal, it is no signal: base-red was not
OBSERVABLE, so no rule downstream of it could work however well written.

TWO RULES THIS DELIBERATELY REFUSES TO IMPLEMENT, because both were measured
unsafe and both look convincing:

1. Cross-batch corroboration of an identical failure. Three batches with
   disjoint single-entry memberships failed with a byte-identical fatal CMake
   error at three different bases; that rule would have exonerated an entry
   whose own follow-up commit admits its head was broken. It is structurally
   indistinguishable from the genuinely innocent case. A streak is therefore
   reported as `suspected` with `safe_to_pause_queue` false -- a prioritisation
   hint, never proof.
2. "No ctest block, therefore infrastructure." A compile or link error produces
   no ctest block either. A failure that names no test contributes an empty set
   and so cannot carry any verdict that names one.

The only evidence that authorises pausing a queue is a failure observed on main
ITSELF, by a job that genuinely ran the suite. `safe_to_pause_queue` is true for
exactly that case and for nothing else.

Read-only. Emits a signal; acts on nothing.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = TOOLS_DIR.parent
sys.path.insert(0, str(TOOLS_DIR / "scripts"))

# Reused rather than restated: the same step-list classifier the receipt-reuse
# guard uses, so "a green that ran nothing" means the same thing in both.
from gate_suite_executed import (  # noqa: E402
    BUILT_BUT_UNTESTED,
    EXECUTED,
    NOT_EXECUTED,
    classify_steps,
)

DEFAULT_REPO = "Generous-Corp/pulp"

SIGNAL_SCHEMA = "base-poison-signal/v1"
# The annotation title Shipyard reads, in the shape `shipyard-receipt-decision`
# already established for a machine-readable CI decision in this workflow.
SIGNAL_TITLE = "base-poison-signal"

STATUS_HEALTHY = "healthy"
STATUS_UNPROVEN = "unproven"
STATUS_SUSPECTED = "suspected"
STATUS_POISONED = "poisoned"

PROOF_NONE = "none"
PROOF_STREAK_ONLY = "batch-streak-only"
PROOF_MAIN_ONLY = "main-observed-failure-without-streak"
PROOF_MAIN_AND_STREAK = "main-observed-failure+batch-streak"

# How many consecutive genuinely-executed batch failures make a streak. Two is
# the smallest number that can distinguish a streak from a single failure at
# all; a degenerate threshold of one would call every ordinary red batch a
# streak and drain the word of meaning.
DEFAULT_MIN_STREAK = 2

# A conclusion below this is not evidence about the tree. `cancelled` in
# particular must never read as healthy: cancelling doomed batches is a
# legitimate way to free lanes, and counting it would let that erase the red.
EVIDENCE_CONCLUSIONS = frozenset({"success", "failure"})

# Naming the wrong fix pull request sends the queue to prioritise an innocent
# branch, so the bar is the attributor's DECISIVE weight (an exact test-name to
# file-stem match), not its ordinary confidence threshold.
_FIX_PR_MIN_STRENGTH = 100


@dataclass(frozen=True)
class SuiteObservation:
    """One run's answer to "did the macOS suite run here, and what failed?"."""

    run_id: str
    lane: str  # "main" | "batch"
    conclusion: str
    execution: str  # EXECUTED | BUILT_BUT_UNTESTED | NOT_EXECUTED
    failing_tests: tuple[str, ...] = ()
    base_sha: str = ""
    head_sha: str = ""
    created_at: str = ""
    # How a `main` observation was obtained. "push-lane" is the designated
    # detector; "tree-identity" is the same tree validated under another event.
    source: str = ""

    @property
    def is_evidence(self) -> bool:
        """True only when a job genuinely ran the suite and reported a result.

        Both halves are load-bearing. A three-step receipt-reuse green reports
        `success` without running anything, and a cancelled run reports nothing
        about the tree at all.
        """
        return (
            self.execution == EXECUTED
            and (self.conclusion or "").strip().lower() in EVIDENCE_CONCLUSIONS
        )

    @property
    def failed(self) -> bool:
        return self.is_evidence and self.conclusion.strip().lower() == "failure"

    @property
    def passed(self) -> bool:
        return self.is_evidence and self.conclusion.strip().lower() == "success"


@dataclass
class Streak:
    """Consecutive genuinely-executed batch failures, and the tests they share."""

    length: int = 0
    shared_tests: tuple[str, ...] = ()
    run_ids: tuple[str, ...] = ()
    distinct_bases: tuple[str, ...] = ()
    skipped_non_evidence: int = 0


def failure_streak(batches: list[SuiteObservation]) -> Streak:
    """Walk batches newest-first and measure the run of executed failures.

    Non-evidence runs are SKIPPED rather than breaking the streak: a cancelled
    or receipt-reuse batch says nothing either way, and treating it as a break
    would let cancelling doomed batches hide the very streak this measures. A
    genuinely-executed PASS does break it -- that is a real observation that the
    tree under it was fine.

    A failure that names no test (a configure, compile or link error) counts
    toward the length but empties the shared set, so no verdict can name a test
    the evidence never named.
    """
    length = 0
    run_ids: list[str] = []
    bases: list[str] = []
    skipped = 0
    shared: frozenset[str] | None = None
    for observation in batches:
        if observation.passed:
            break
        if not observation.is_evidence:
            skipped += 1
            continue
        length += 1
        run_ids.append(observation.run_id)
        if observation.base_sha and observation.base_sha not in bases:
            bases.append(observation.base_sha)
        names = frozenset(observation.failing_tests)
        shared = names if shared is None else (shared & names)
    return Streak(
        length=length,
        shared_tests=tuple(sorted(shared or frozenset())),
        run_ids=tuple(run_ids),
        distinct_bases=tuple(bases),
        skipped_non_evidence=skipped,
    )


@dataclass
class Verdict:
    status: str = STATUS_UNPROVEN
    proof: str = PROOF_NONE
    tests: tuple[str, ...] = ()
    main_observed: bool = False
    main_run_id: str = ""
    main_conclusion: str = ""
    main_failing_tests: tuple[str, ...] = ()
    main_evidence_source: str = ""
    streak: Streak = field(default_factory=Streak)
    min_streak: int = DEFAULT_MIN_STREAK
    reason: str = ""

    @property
    def safe_to_pause_queue(self) -> bool:
        """Only a failure observed on main itself authorises pausing the queue."""
        return self.status == STATUS_POISONED


def detect(
    main: SuiteObservation | None,
    batches: list[SuiteObservation],
    min_streak: int = DEFAULT_MIN_STREAK,
) -> Verdict:
    """Classify the base from a main observation and the batch history.

    Fails closed in every direction: absence of evidence is `unproven`, never
    `healthy`; a streak alone is `suspected`, never `poisoned`; and `poisoned`
    requires main and the streak to agree on a NAMED test.
    """
    if min_streak < 2:
        raise ValueError(
            f"min_streak must be at least 2; {min_streak} would call a single "
            "failure a streak"
        )
    streak = failure_streak(batches)
    verdict = Verdict(streak=streak, min_streak=min_streak)

    if main is not None:
        verdict.main_run_id = main.run_id
        verdict.main_conclusion = main.conclusion
        verdict.main_evidence_source = main.source
        verdict.main_observed = main.is_evidence
        if main.is_evidence:
            verdict.main_failing_tests = tuple(sorted(set(main.failing_tests)))

    if not verdict.main_observed:
        reason = (
            "no push-to-main run genuinely executed the macOS suite, so the "
            "base's own health is unmeasured"
        )
        if streak.length >= min_streak and streak.shared_tests:
            verdict.status = STATUS_SUSPECTED
            verdict.proof = PROOF_STREAK_ONLY
            verdict.tests = streak.shared_tests
            verdict.reason = (
                f"{streak.length} consecutive executed batches share "
                f"{len(streak.shared_tests)} failing test(s), but {reason}. A "
                "shared failure across batches is NOT proof the base owns it."
            )
        else:
            verdict.status = STATUS_UNPROVEN
            verdict.reason = reason
        return verdict

    if main.passed:
        verdict.status = STATUS_HEALTHY
        verdict.reason = (
            f"push run {main.run_id} executed the macOS suite on main and passed"
        )
        return verdict

    named = tuple(sorted(set(verdict.main_failing_tests) & set(streak.shared_tests)))
    if named and streak.length >= min_streak:
        verdict.status = STATUS_POISONED
        verdict.proof = PROOF_MAIN_AND_STREAK
        verdict.tests = named
        verdict.reason = (
            f"main's own suite failed {', '.join(named)} in run {main.run_id}, and "
            f"the same test(s) failed in {streak.length} consecutive executed "
            "batches. Every batch re-formed over this base inherits it."
        )
        return verdict

    verdict.status = STATUS_SUSPECTED
    verdict.proof = PROOF_MAIN_ONLY
    verdict.tests = verdict.main_failing_tests
    if not verdict.main_failing_tests:
        verdict.reason = (
            f"push run {main.run_id} failed on main but named no ctest, so the "
            "failure is a configure, build or link error and no test can be "
            "named for it"
        )
    else:
        verdict.reason = (
            f"main's own suite failed in run {main.run_id}, but the failure is "
            f"not yet shared by {min_streak} consecutive executed batches "
            f"(streak {streak.length})"
        )
    return verdict


def signal(
    verdict: Verdict,
    candidate_fix_pr: int | None = None,
    likely_culprits: dict[int, list[str]] | None = None,
) -> dict:
    """The machine-readable record. Every consumer reads `safe_to_pause_queue`.

    `likely_culprits` is the batch-membership finding: the queued entry whose
    presence separates the batches that failed a test from the ones that ran it
    green. It names a PR to dequeue, never a fix to prioritise, so it is kept
    apart from `candidate_fix_pr`.
    """
    return {
        "schema": SIGNAL_SCHEMA,
        "status": verdict.status,
        "proof": verdict.proof,
        "safe_to_pause_queue": verdict.safe_to_pause_queue,
        "tests": list(verdict.tests),
        "main_observed": verdict.main_observed,
        "main_run_id": verdict.main_run_id or None,
        "main_conclusion": verdict.main_conclusion or None,
        "main_evidence_source": verdict.main_evidence_source or None,
        "main_failing_tests": list(verdict.main_failing_tests),
        "batch_streak": verdict.streak.length,
        "batch_streak_tests": list(verdict.streak.shared_tests),
        "batch_streak_runs": list(verdict.streak.run_ids),
        "batch_streak_distinct_bases": list(verdict.streak.distinct_bases),
        "batch_non_evidence_skipped": verdict.streak.skipped_non_evidence,
        "min_streak": verdict.min_streak,
        "candidate_fix_pr": candidate_fix_pr,
        "likely_culprits": [
            {"pr": pr, "tests": sorted(tests)}
            for pr, tests in sorted((likely_culprits or {}).items())
        ],
        "reason": " ".join(verdict.reason.split()),
    }


def _escape_command_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def render(payload: dict) -> tuple[list[str], str]:
    """Annotation lines Shipyard reads, and a job-summary table people read."""
    compact = json.dumps(payload, separators=(",", ":"), sort_keys=True)
    lines = [f"::notice title={SIGNAL_TITLE}::" + _escape_command_data(compact)]
    tests = ", ".join(payload["tests"]) or "—"
    fix = payload["candidate_fix_pr"]
    culprits = (
        "; ".join(
            f"#{entry['pr']} ({', '.join(entry['tests'])})"
            for entry in payload.get("likely_culprits") or []
        )
        or "—"
    ).replace("|", chr(92) + "|")
    md = [
        "### Base health",
        "",
        "| Field | Value |",
        "|---|---|",
        f"| status | `{payload['status']}` |",
        f"| proof | `{payload['proof']}` |",
        f"| safe to pause queue | `{str(payload['safe_to_pause_queue']).lower()}` |",
        f"| test(s) | {tests} |",
        f"| main observation | {payload['main_run_id'] or 'none'} "
        f"({payload['main_conclusion'] or 'unobserved'}"
        f"{', via ' + payload['main_evidence_source'] if payload['main_evidence_source'] else ''}) |",
        f"| batch streak | {payload['batch_streak']} (min {payload['min_streak']}) |",
        f"| candidate fix PR | {('#%d' % fix) if fix else '—'} |",
        f"| likely culprit PR | {culprits} |",
        f"| reason | {payload['reason'].replace('|', chr(92) + '|')} |",
        "",
    ]
    if payload["status"] == STATUS_SUSPECTED:
        md.append(
            "> `suspected` is a prioritisation hint, not proof. A failure shared "
            "across batches is structurally indistinguishable from one entry "
            "breaking every batch it joins, so it must not pause the queue.\n"
        )
    return lines, "\n".join(md) + "\n"


# --------------------------------------------------------------------------
# Network adapters. Thin by design: every decision above is pure and tested.
# --------------------------------------------------------------------------


# `ghapp` locally, `gh` on a GitHub runner where no App wrapper exists. The
# override is read from the environment rather than argued so every helper -
# including the attributor this reuses - resolves the same binary.
GH_CLI_ENV = "PULP_GH_CLI"


def gh_cli() -> str:
    return (os.environ.get(GH_CLI_ENV) or "").strip() or "ghapp"


def gh(path: str, jq: str | None = None) -> str | None:
    cmd = [gh_cli(), "api", path] + (["--jq", jq] if jq else [])
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.stdout.strip() if proc.returncode == 0 else None


def gh_bytes(path: str) -> bytes | None:
    proc = subprocess.run([gh_cli(), "api", path], capture_output=True)
    return proc.stdout if proc.returncode == 0 and proc.stdout else None


LAST_TESTS_FAILED_RE = re.compile(r"^\s*\d+\s*:\s*(\S.*?)\s*$")


def parse_last_tests_failed(text: str) -> tuple[str, ...]:
    """ctest's `LastTestsFailed.log`: `<index>:<test name>` per line.

    Absent or empty means the failure happened before ctest named a test, which
    is a fact about the failure rather than a gap to paper over.
    """
    names: list[str] = []
    for line in text.splitlines():
        match = LAST_TESTS_FAILED_RE.match(line)
        if match and match.group(1) not in names:
            names.append(match.group(1))
    return tuple(names)


# `ghapp api .../actions/jobs/<id>/logs` refuses any response carrying terminal
# escape sequences and returns 99 bytes of refusal, so a log scrape of the test
# step silently yields NO failing tests -- which reads as "the failure was not a
# test failure". The uploaded ctest artifact is the machine-readable record and
# is written with `if: always()`, so it exists for exactly the runs that matter.
CTEST_ARTIFACT_PREFIX = "ctest-logs-"
LAST_TESTS_FAILED_MEMBER = "Testing/Temporary/LastTestsFailed.log"


def artifact_failing_tests(repo: str, run_id: str, key: str = "macos") -> tuple[str, ...]:
    raw = gh(
        f"repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100",
        f'[.artifacts[]|select(.name=="{CTEST_ARTIFACT_PREFIX}{key}" and '
        "(.expired|not))]|.[0].id",
    )
    if not raw or not raw.strip().isdigit():
        return ()
    blob = gh_bytes(f"repos/{repo}/actions/artifacts/{raw.strip()}/zip")
    if not blob:
        return ()
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            with archive.open(LAST_TESTS_FAILED_MEMBER) as member:
                return parse_last_tests_failed(member.read().decode("utf-8", "replace"))
    except (zipfile.BadZipFile, KeyError):
        return ()


# On `push` the macOS leg keeps its descriptive matrix name so it detects
# without claiming the required context; on the gate events it is renamed
# `macos`. Both spellings are the same leg.
# Bare `macos`, or the matrix spelling `macOS (ARM64) [local]`. The alias
# placeholders `macos-pr-unused` / `macos-merge-unused` must NOT match: they
# report a context without owning a leg.
MACOS_JOB_RE = re.compile(r"^macos(?:\s*\(|$)", re.IGNORECASE)


def _macos_jobs(repo: str, run_id: str) -> list[dict]:
    raw = gh(f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100")
    if not raw:
        return []
    try:
        jobs = json.loads(raw).get("jobs", [])
    except json.JSONDecodeError:
        return []
    return [j for j in jobs if MACOS_JOB_RE.match((j.get("name") or "").strip())]


def _execution(jobs: list[dict]) -> str:
    verdicts = [classify_steps(j.get("steps") or []) for j in jobs]
    for preferred in (EXECUTED, BUILT_BUT_UNTESTED):
        if preferred in verdicts:
            return preferred
    return NOT_EXECUTED


def observe(repo: str, run: dict, lane: str) -> SuiteObservation:
    run_id = str(run.get("id"))
    jobs = _macos_jobs(repo, run_id)
    execution = _execution(jobs)
    conclusion = (run.get("conclusion") or "").strip().lower()
    # A run whose macOS leg failed but whose own conclusion is still pending is
    # not yet evidence; read the leg rather than the run where they disagree.
    leg = [(j.get("conclusion") or "").strip().lower() for j in jobs]
    if execution == EXECUTED and "failure" in leg:
        conclusion = "failure"
    failing: tuple[str, ...] = ()
    if execution == EXECUTED and conclusion == "failure":
        failing = artifact_failing_tests(repo, run_id)
    return SuiteObservation(
        run_id=run_id,
        lane=lane,
        conclusion=conclusion,
        execution=execution,
        failing_tests=failing,
        base_sha=_base_sha(run),
        head_sha=(run.get("head_sha") or ""),
        created_at=(run.get("created_at") or ""),
    )


BATCH_BRANCH_RE = re.compile(r"^gh-readonly-queue/[^/]+/pr-\d+-([0-9a-f]{40})$")


def _base_sha(run: dict) -> str:
    """A merge-queue ref names the base it was built on; a push run is its own."""
    match = BATCH_BRANCH_RE.match((run.get("head_branch") or "").strip())
    return match.group(1) if match else (run.get("head_sha") or "")


def _runs(repo: str, event: str, limit: int, branch: str = "") -> list[dict]:
    query = f"event={event}&per_page={min(limit, 100)}"
    if branch:
        query += f"&branch={branch}"
    raw = gh(f"repos/{repo}/actions/workflows/build.yml/runs?{query}")
    if not raw:
        return []
    try:
        runs = json.loads(raw).get("workflow_runs", [])
    except json.JSONDecodeError:
        return []
    return [r for r in runs if (r.get("status") or "") == "completed"][:limit]


SOURCE_PUSH_LANE = "push-lane"
SOURCE_TREE_IDENTITY = "tree-identity"


def _tree_sha(repo: str, commit_sha: str) -> str:
    return (gh(f"repos/{repo}/commits/{commit_sha}", ".commit.tree.sha") or "").strip()


def main_head(repo: str) -> tuple[str, str]:
    """main's current head sha and the tree it points at."""
    raw = gh(f"repos/{repo}/commits/main", "[.sha,.commit.tree.sha]|@tsv")
    if not raw or "\t" not in raw:
        return ("", "")
    sha, tree = raw.split("\t", 1)
    return (sha.strip(), tree.strip())


def observe_main_push_lane(repo: str, limit: int = 20) -> SuiteObservation | None:
    """The newest push-to-main run whose macOS leg genuinely executed the suite.

    The designated detector. It reports nothing while every push run is
    cancelled while pending, which is why the tree-identity route below exists.
    """
    for run in _runs(repo, "push", limit, branch="main"):
        seen = observe(repo, run, "main")
        if seen.is_evidence:
            return SuiteObservation(**{**seen.__dict__, "source": SOURCE_PUSH_LANE})
    return None


def observe_main_by_tree(
    repo: str, events: tuple[str, ...] = ("merge_group", "push"), limit: int = 20
) -> SuiteObservation | None:
    """A main observation with no new build: the same TREE, validated elsewhere.

    A merge queue validates `main` plus its entries as one commit, and the
    commit that lands carries that commit's tree. So when main's head tree
    equals the head tree of a run whose macOS leg genuinely executed, that run
    built and tested main's exact tree -- the same sources, the same binaries,
    the same test result. A failure there is a failure observed on main itself,
    and it costs two commit reads rather than a ~40-minute gate lane.

    Tree identity, not sha identity: the queue's merge method may produce a
    landing commit whose sha differs from the group commit while the tree it
    points at is the same, and it is the tree that determines what was built.
    """
    _, tree = main_head(repo)
    if not tree:
        return None
    for event in events:
        branch = "main" if event == "push" else ""
        for run in _runs(repo, event, limit, branch=branch):
            head = (run.get("head_sha") or "").strip()
            if not head or _tree_sha(repo, head) != tree:
                continue
            seen = observe(repo, run, "main")
            if seen.is_evidence:
                return SuiteObservation(
                    **{**seen.__dict__, "source": SOURCE_TREE_IDENTITY}
                )
    return None


def observe_main(repo: str, limit: int = 20) -> SuiteObservation | None:
    """main's health, from the designated lane first and tree identity second."""
    return observe_main_push_lane(repo, limit) or observe_main_by_tree(
        repo, limit=limit
    )


def observe_batches(repo: str, limit: int = 12) -> list[SuiteObservation]:
    return [observe(repo, run, "batch") for run in _runs(repo, "merge_group", limit)]


def decisive_fix_pr(attribution) -> int | None:
    """The one pull request an attribution DECISIVELY names, or nothing.

    Two refusals, both deliberate. Below the decisive weight (an exact
    test-name to file-stem match) the attributor's own comment calls a finding
    a false accusation, and this consumer is stricter still than its ordinary
    confidence threshold: a named fix tells a queue which branch to prioritise,
    and prioritising an innocent one leaves the real break in place while
    looking like progress. More than one contender at the top strength is a
    coin toss, so it names nobody rather than picking the lowest number.
    """
    if attribution is None:
        return None
    if attribution.best_strength < _FIX_PR_MIN_STRENGTH:
        return None
    if len(attribution.contenders) != 1:
        return None
    return attribution.culprit


def candidate_fix_pr(repo: str, tests: tuple[str, ...]) -> int | None:
    """An OPEN pull request whose diff decisively owns one of these tests.

    Uses the queue attributor's own scoring rather than a second copy of it.
    """
    if not tests:
        return None
    try:
        import queue_batch_attribute as attributor
    except ImportError:
        return None
    numbers = attributor.open_prs(repo)
    if not numbers:
        return None
    files = {n: attributor.pr_files(repo, n) for n in numbers}
    roots = attributor.census_include_roots(REPO_ROOT)
    return decisive_fix_pr(
        attributor.attribute(list(tests), files, census_roots=roots)
    )


def membership_culprits(repo: str, since: str, limit: int) -> dict[int, list[str]]:
    """Queued entries the merge-group history names as breaking a test.

    File ownership cannot see a change that breaks a test it never names; the
    queue's overlapping batch memberships can. Reuses the attributor's rule.
    """
    try:
        import queue_batch_attribute as attributor
    except ImportError:
        return {}
    read = attributor.observe_history(
        repo,
        since=since,
        max_runs=limit,
        cache_dir=attributor.default_cache_dir(repo),
    )
    return attributor.likely_culprits(
        attributor.history_attribution(read.observations)
    )


def _emit(payload: dict, output: str | None) -> None:
    lines, markdown = render(payload)
    for line in lines:
        print(line)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(markdown)
    else:
        print(markdown, end="")
    if output:
        Path(output).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--min-streak", type=int, default=DEFAULT_MIN_STREAK)
    parser.add_argument("--batch-limit", type=int, default=12)
    parser.add_argument("--main-limit", type=int, default=20)
    parser.add_argument("--output", default="", help="write the signal JSON here")
    parser.add_argument(
        "--name-fix-pr",
        action="store_true",
        help="also look for the open pull request that decisively owns the test, "
        "and for the queued entry the batch-membership history names as the culprit",
    )
    parser.add_argument(
        "--history-since",
        default="24h",
        help="window --name-fix-pr reads for batch-membership culprits",
    )
    parser.add_argument(
        "--history-limit",
        type=int,
        default=60,
        help="most merge_group runs that window may hold",
    )
    parser.add_argument(
        "--fail-on-poisoned",
        action="store_true",
        help="exit 1 when the base is proven poisoned (default: report only)",
    )
    args = parser.parse_args(argv)

    verdict = detect(
        observe_main(args.repo, args.main_limit),
        observe_batches(args.repo, args.batch_limit),
        min_streak=args.min_streak,
    )
    fix = candidate_fix_pr(args.repo, verdict.tests) if args.name_fix_pr else None
    culprits = (
        membership_culprits(args.repo, args.history_since, args.history_limit)
        if args.name_fix_pr
        else None
    )
    payload = signal(verdict, fix, culprits)
    _emit(payload, args.output or None)
    if args.fail_on_poisoned and payload["safe_to_pause_queue"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
