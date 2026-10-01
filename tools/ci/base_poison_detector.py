#!/usr/bin/env python3
"""Decide whether `main` itself is carrying the failure a merge queue keeps ejecting batches for.

WHAT THIS EXISTS TO STOP. A merge_group batch is `main` plus the queued entries.
When main carries a failing test, every batch inherits it, fails after a full
gate, ejects its innocent entries, re-forms, and pays again. Nothing in the
batch's own report says the base is the cause, so each batch is read as a new
culprit in turn.

WHERE MAIN'S OWN HEALTH COMES FROM. The queue lands with the MERGE method, so
the commit on main IS the merge-group head: the merge_group run on main's tip
sha already built and tested main's exact commit. The observation is EVERY
required status context on that commit (`[governance] required_status_checks`),
gathered from every workflow that produces one (`[landability] workflows`) plus
the commit's statuses -- the job that owns each context, never a run's
conclusion. A run's conclusion folds in advisory legs (hosted Linux fails
routinely), so reading it called a green tip red; and judging one context
alone missed a fourteen-hour red of the required `drift-fast` context while
`macos` stayed green. A push-to-main run carries no macOS leg and is not an
observation.

When the tip has no merge_group run (an admin or direct push) main's health is
`unproven`, never inferred. A required context that is still running or never
reported leaves it `unproven` too; one that failed makes it red whatever the
others say.

A batch is judged the same way, so a batch counts toward a streak when ANY
required context failed on it, and its failing tests come from wherever that
context records them: the `ctest-logs-macos` artifact for `macos`, the job's own
log for a context (such as `drift-fast`) that uploads no artifact.

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


# The one required context whose green means "the test suite ran": its job's
# steps are classified, so a receipt-reuse green that ran nothing is not
# evidence. Every other context is a job (or status) whose conclusion is its
# whole answer.
SUITE_CONTEXT = "macos"

SOURCE_JOB = "job"
SOURCE_STATUS = "status"
SOURCE_MISSING = "missing"


@dataclass(frozen=True)
class ContextResult:
    """One required status context on one commit."""

    context: str
    conclusion: str  # success | failure | cancelled | skipped | "" (running / absent)
    source: str = SOURCE_JOB  # job | status | missing
    execution: str = ""  # only for SUITE_CONTEXT
    run_id: str = ""
    failing_tests: tuple[str, ...] = ()

    @property
    def is_suite(self) -> bool:
        return self.context == SUITE_CONTEXT

    @property
    def failed(self) -> bool:
        """A red this context genuinely reported about the tree.

        The suite context additionally has to have run the suite: a macos
        failure before ctest (a build that never reached the Test step) is
        classified by `gate_suite_executed` and is not treated as evidence here,
        exactly as before.
        """
        if (self.conclusion or "").strip().lower() != "failure":
            return False
        return not self.is_suite or self.execution == EXECUTED

    @property
    def passed(self) -> bool:
        if (self.conclusion or "").strip().lower() != "success":
            return False
        return not self.is_suite or self.execution == EXECUTED

    @property
    def state(self) -> str:
        """A one-word label for reasons and the summary table."""
        if self.source == SOURCE_MISSING:
            return "missing"
        if self.failed:
            return "failure"
        if self.passed:
            return "success"
        if self.is_suite and (self.conclusion or "") == "success":
            return f"success-but-{self.execution or NOT_EXECUTED}"
        return self.conclusion or "pending"

    def as_json(self) -> dict:
        return {
            "context": self.context,
            "state": self.state,
            "conclusion": self.conclusion or None,
            "source": self.source,
            "run_id": self.run_id or None,
            "tests": list(self.failing_tests),
        }


@dataclass(frozen=True)
class SuiteObservation:
    """One commit's answer to "did every required context report, and what failed?".

    Built from `contexts` when the per-context read is available. The legacy
    single-gate shape (no contexts) keeps its old meaning: `conclusion` and
    `execution` of the macOS gate alone.
    """

    run_id: str
    lane: str  # "main" | "batch"
    conclusion: str
    execution: str  # EXECUTED | BUILT_BUT_UNTESTED | NOT_EXECUTED
    failing_tests: tuple[str, ...] = ()
    base_sha: str = ""
    head_sha: str = ""
    created_at: str = ""
    # How a `main` observation was obtained: "head-sha" is the merge_group run
    # on main's tip itself; "tree-identity" is the same tree validated earlier.
    source: str = ""
    contexts: tuple[ContextResult, ...] = ()

    @property
    def failing_contexts(self) -> tuple[str, ...]:
        return tuple(c.context for c in self.contexts if c.failed)

    @property
    def is_evidence(self) -> bool:
        """True only when the commit's required gate genuinely reported a result.

        A red on ANY required context is evidence on its own: a drift check that
        failed says the tree is broken whatever the suite is still doing. A
        green needs every required context green AND the suite to have run: a
        three-step receipt-reuse green reports `success` without running
        anything, and a cancelled or still-running context reports nothing.
        """
        if self.contexts:
            return self.failed or self.passed
        return (
            self.execution == EXECUTED
            and (self.conclusion or "").strip().lower() in EVIDENCE_CONCLUSIONS
        )

    @property
    def failed(self) -> bool:
        if self.contexts:
            return any(c.failed for c in self.contexts)
        return self.is_evidence and self.conclusion.strip().lower() == "failure"

    @property
    def passed(self) -> bool:
        if self.contexts:
            return all(c.passed for c in self.contexts)
        return self.is_evidence and self.conclusion.strip().lower() == "success"

    def unresolved(self) -> str:
        """Why a non-evidence observation is not evidence, context by context."""
        if not self.contexts:
            return (
                f"its required gate reads {self.conclusion or 'pending'} with "
                f"suite execution {self.execution}"
            )
        waiting = [f"{c.context} {c.state}" for c in self.contexts if not c.passed]
        return "required context(s) not yet green: " + ", ".join(waiting)


@dataclass
class Streak:
    """Consecutive genuinely-executed batch failures, and the tests they share."""

    length: int = 0
    shared_tests: tuple[str, ...] = ()
    run_ids: tuple[str, ...] = ()
    distinct_bases: tuple[str, ...] = ()
    skipped_non_evidence: int = 0
    # The required contexts every batch in the streak failed, and the batch
    # heads it spans. Both are empty for legacy single-gate observations.
    shared_contexts: tuple[str, ...] = ()
    heads: tuple[str, ...] = ()


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
    heads: list[str] = []
    skipped = 0
    shared: frozenset[str] | None = None
    shared_ctx: frozenset[str] | None = None
    for observation in batches:
        if observation.passed:
            break
        if not observation.is_evidence:
            skipped += 1
            continue
        length += 1
        run_ids.append(observation.run_id)
        if observation.head_sha and observation.head_sha not in heads:
            heads.append(observation.head_sha)
        if observation.base_sha and observation.base_sha not in bases:
            bases.append(observation.base_sha)
        names = frozenset(observation.failing_tests)
        shared = names if shared is None else (shared & names)
        failing = frozenset(observation.failing_contexts)
        shared_ctx = failing if shared_ctx is None else (shared_ctx & failing)
    return Streak(
        length=length,
        shared_tests=tuple(sorted(shared or frozenset())),
        run_ids=tuple(run_ids),
        distinct_bases=tuple(bases),
        skipped_non_evidence=skipped,
        shared_contexts=tuple(sorted(shared_ctx or frozenset())),
        heads=tuple(heads),
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
    # The main commit the verdict is about, whether or not it was observed.
    main_head_sha: str = ""
    # Every required context on main's tip, and the ones that failed there.
    main_contexts: tuple[ContextResult, ...] = ()
    main_failing_contexts: tuple[str, ...] = ()
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
    tip_sha: str = "",
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
    verdict = Verdict(streak=streak, min_streak=min_streak, main_head_sha=tip_sha)

    if main is not None:
        verdict.main_head_sha = tip_sha or main.head_sha
        verdict.main_run_id = main.run_id
        verdict.main_conclusion = main.conclusion
        verdict.main_evidence_source = main.source
        verdict.main_observed = main.is_evidence
        verdict.main_contexts = main.contexts
        if main.is_evidence:
            verdict.main_failing_tests = tuple(sorted(set(main.failing_tests)))
            verdict.main_failing_contexts = main.failing_contexts

    if not verdict.main_observed:
        tip = f" {verdict.main_head_sha[:12]}" if verdict.main_head_sha else ""
        if main is None:
            reason = (
                f"main's tip{tip} has no merge_group run whose required gate "
                "genuinely executed the suite (an admin or direct push lands "
                "none), so the base's own health is unmeasured"
            )
        else:
            reason = (
                f"run {main.run_id} on main's tip{tip} is not evidence yet: "
                f"{main.unresolved()}, so the base's own health is unmeasured"
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
        if main.contexts:
            verdict.reason = (
                f"all {len(main.contexts)} required context(s) passed on main's "
                f"tip {verdict.main_head_sha[:12]}, and run {main.run_id} "
                "executed the macOS suite"
            )
        else:
            verdict.reason = (
                f"run {main.run_id} executed the macOS suite on main's tip "
                f"{verdict.main_head_sha[:12]} and its required gate passed"
            )
        return verdict

    named = tuple(sorted(set(verdict.main_failing_tests) & set(streak.shared_tests)))
    if named and streak.length >= min_streak:
        verdict.status = STATUS_POISONED
        verdict.proof = PROOF_MAIN_AND_STREAK
        verdict.tests = named
        verdict.reason = (
            f"main's own gate failed {', '.join(named)} in run {main.run_id}, and "
            f"the same test(s) failed in {streak.length} consecutive executed "
            "batches. Every batch re-formed over this base inherits it."
        )
        return verdict

    verdict.status = STATUS_SUSPECTED
    verdict.proof = PROOF_MAIN_ONLY
    verdict.tests = verdict.main_failing_tests
    where = (
        f"required context(s) {', '.join(verdict.main_failing_contexts)} failed "
        f"on main's tip {verdict.main_head_sha[:12]} (run {main.run_id})"
        if verdict.main_failing_contexts
        else f"main's own suite failed in run {main.run_id}"
    )
    if not verdict.main_failing_tests:
        verdict.reason = (
            f"{where} but named no ctest, so the failure is a configure, build "
            "or link error (or a check that is not a ctest) and no test can be "
            "named for it"
        )
    else:
        verdict.reason = (
            f"{where}, but the failure is not yet shared by {min_streak} "
            f"consecutive executed batches (streak {streak.length})"
        )
    return verdict


REQUIRED_SOURCE_PROTECTION = "protection"
REQUIRED_SOURCE_FALLBACK = "macos-only-fallback"


def signal(
    verdict: Verdict,
    candidate_fix_pr: int | None = None,
    likely_culprits: dict[int, list[str]] | None = None,
    required_contexts_source: str | None = None,
) -> dict:
    """The machine-readable record. Every consumer reads `safe_to_pause_queue`.

    `likely_culprits` is the batch-membership finding: the queued entry whose
    presence separates the batches that failed a test from the ones that ran it
    green. It names a PR to dequeue, never a fix to prioritise, so it is kept
    apart from `candidate_fix_pr`. `required_contexts_source` says which
    required set that finding judged: `macos-only-fallback` means branch
    protection could not be read and every other required context went unseen.
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
        "main_head_sha": verdict.main_head_sha or None,
        "main_failing_tests": list(verdict.main_failing_tests),
        "batch_streak": verdict.streak.length,
        "batch_streak_tests": list(verdict.streak.shared_tests),
        "batch_streak_runs": list(verdict.streak.run_ids),
        "batch_streak_distinct_bases": list(verdict.streak.distinct_bases),
        "batch_non_evidence_skipped": verdict.streak.skipped_non_evidence,
        "batch_streak_contexts": list(verdict.streak.shared_contexts),
        "batch_streak_heads": list(verdict.streak.heads),
        "main_failing_contexts": list(verdict.main_failing_contexts),
        "main_contexts": [c.as_json() for c in verdict.main_contexts],
        "min_streak": verdict.min_streak,
        "candidate_fix_pr": candidate_fix_pr,
        "likely_culprits": [
            {"pr": pr, "tests": sorted(tests)}
            for pr, tests in sorted((likely_culprits or {}).items())
        ],
        "required_contexts_source": required_contexts_source,
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
    source = payload.get("required_contexts_source")
    required_row = {
        None: "—",
        REQUIRED_SOURCE_PROTECTION: "every required context (from branch protection)",
        REQUIRED_SOURCE_FALLBACK: "**macos only**: branch protection was unreadable, "
        "so every other required context went unjudged",
    }.get(source, str(source))
    contexts_row = (
        "; ".join(
            f"{entry['context']}: {entry['state']}"
            for entry in payload.get("main_contexts") or []
        )
        or "—"
    ).replace("|", chr(92) + "|")
    shared_ctx = payload.get("batch_streak_contexts") or []
    streak_ctx = f", every batch failed {', '.join(shared_ctx)}" if shared_ctx else ""
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
        f"| main tip | {(payload.get('main_head_sha') or '')[:12] or 'unknown'} |",
        f"| failing context(s) on main | "
        f"{', '.join(payload.get('main_failing_contexts') or []) or '—'} |",
        f"| required contexts on main | {contexts_row} |",
        f"| batch streak | {payload['batch_streak']} (min {payload['min_streak']})"
        f"{streak_ctx} |",
        f"| candidate fix PR | {('#%d' % fix) if fix else '—'} |",
        f"| likely culprit PR | {culprits} |",
        f"| required contexts judged | {required_row} |",
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


# The matrix spelling of the macOS leg, `macOS (ARM64) [local]`, and the bare
# `macos`. The alias placeholders `macos-pr-unused` / `macos-merge-unused` must
# NOT match: they report a context without owning a leg. Used only when no job
# carries a required context name.
MACOS_JOB_RE = re.compile(r"^macos(?:\s*\(|$)", re.IGNORECASE)

SHIPYARD_CONFIG = REPO_ROOT / ".shipyard" / "config.toml"
# Only what a required gate can be named when the config cannot be read.
FALLBACK_REQUIRED_CONTEXTS = ("macos",)


def required_contexts(config: Path = SHIPYARD_CONFIG) -> tuple[str, ...]:
    """The required status checks, from `[governance] required_status_checks`.

    That list mirrors the live ruleset, and only the contexts it names decide
    whether main is healthy: an advisory leg's red never ejects a batch and
    must never redden the base.

    It is the whole contract, so it names contexts several workflows produce
    (`drift-fast`, the Vellum freezes, the WebCLAP job, the version gate) as
    well as build.yml's `macos`. Every one of them is judged: a required
    context that failed ejects the batch exactly as a red `macos` does.
    """
    names = _config_list("governance", "required_status_checks", config)
    return names or FALLBACK_REQUIRED_CONTEXTS


BUILD_WORKFLOW = ".github/workflows/build.yml"
# Only the workflow that owns `macos` can be named when the config cannot be
# read, matching the macos-only required fallback.
FALLBACK_WORKFLOWS = (BUILD_WORKFLOW,)


def required_workflows(config: Path = SHIPYARD_CONFIG) -> tuple[str, ...]:
    """Every workflow that produces a required context, from `[landability] workflows`.

    Shipyard's landability preflight reads the same list, so the detector looks
    for a required context in exactly the workflows the landing path expects
    to request it from.
    """
    names = _config_list("landability", "workflows", config)
    if not names:
        return FALLBACK_WORKFLOWS
    return names if BUILD_WORKFLOW in names else (BUILD_WORKFLOW, *names)


def _config_list(table: str, key: str, config: Path) -> tuple[str, ...]:
    try:
        import tomllib
    except ImportError:  # Python < 3.11
        return ()
    try:
        data = tomllib.loads(config.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return ()
    names = (data.get(table) or {}).get(key)
    if not isinstance(names, list):
        return ()
    return tuple(n.strip() for n in names if isinstance(n, str) and n.strip())


def gate_jobs(jobs: list[dict], required: tuple[str, ...]) -> list[dict]:
    """The jobs that own a required context in this run.

    A job named exactly as a required context owns it (the merge-group and PR
    `macos` leg). Failing that, the macOS matrix leg under its descriptive
    name. Never the advisory Linux or Windows legs.
    """
    wanted = {name.strip() for name in required}
    owned = [j for j in jobs if (j.get("name") or "").strip() in wanted]
    if owned:
        return owned
    return [j for j in jobs if MACOS_JOB_RE.match((j.get("name") or "").strip())]


def _jobs(repo: str, run_id: str) -> list[dict]:
    raw = gh(f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100")
    if not raw:
        return []
    try:
        return json.loads(raw).get("jobs", [])
    except json.JSONDecodeError:
        return []


def _execution(jobs: list[dict]) -> str:
    verdicts = [classify_steps(j.get("steps") or []) for j in jobs]
    for preferred in (EXECUTED, BUILT_BUT_UNTESTED):
        if preferred in verdicts:
            return preferred
    return NOT_EXECUTED


def judge_gate(jobs: list[dict], required: tuple[str, ...]) -> tuple[str, str]:
    """(execution, conclusion) of the required gate, never of the run.

    Failure if any gate job failed; success only when every gate job
    succeeded; otherwise the first non-empty job conclusion (cancelled,
    skipped) or "" while a job is still running -- neither is evidence.
    """
    gate = gate_jobs(jobs, required)
    execution = _execution(gate)
    leg = [(j.get("conclusion") or "").strip().lower() for j in gate]
    if "failure" in leg:
        conclusion = "failure"
    elif leg and all(c == "success" for c in leg):
        conclusion = "success"
    else:
        conclusion = next((c for c in leg if c and c != "success"), "")
    return execution, conclusion


def observe(
    repo: str,
    run: dict,
    lane: str,
    required: tuple[str, ...] | None = None,
    source: str = "",
) -> SuiteObservation:
    run_id = str(run.get("id"))
    execution, conclusion = judge_gate(
        _jobs(repo, run_id), required or required_contexts()
    )
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
        source=source,
    )


BATCH_BRANCH_RE = re.compile(r"^gh-readonly-queue/[^/]+/pr-\d+-([0-9a-f]{40})$")


def _base_sha(run: dict) -> str:
    """A merge-queue ref names the base it was built on; a push run is its own."""
    match = BATCH_BRANCH_RE.match((run.get("head_branch") or "").strip())
    return match.group(1) if match else (run.get("head_sha") or "")


def _runs(
    repo: str,
    event: str,
    limit: int,
    branch: str = "",
    head_sha: str = "",
    completed_only: bool = True,
) -> list[dict]:
    query = f"event={event}&per_page={min(limit, 100)}"
    if branch:
        query += f"&branch={branch}"
    if head_sha:
        query += f"&head_sha={head_sha}"
    raw = gh(f"repos/{repo}/actions/workflows/build.yml/runs?{query}")
    if not raw:
        return []
    try:
        runs = json.loads(raw).get("workflow_runs", [])
    except json.JSONDecodeError:
        return []
    if completed_only:
        runs = [r for r in runs if (r.get("status") or "") == "completed"]
    return runs[:limit]


SOURCE_HEAD_SHA = "head-sha"
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


def newest_run(runs: list[dict]) -> dict | None:
    """The newest run by creation time; the API already reports each run's
    latest attempt."""
    if not runs:
        return None
    return max(runs, key=lambda r: ((r.get("created_at") or ""), int(r.get("id") or 0)))


def judge_contexts(
    jobs: list[dict],
    statuses: list[dict],
    required: tuple[str, ...],
) -> tuple[ContextResult, ...]:
    """Every required context on one commit, from its jobs first and statuses second.

    A job named exactly as a context owns it; for `macos` the descriptive matrix
    leg is the fallback owner, and its steps decide whether the suite ran. A
    context no job owns is read from the commit's statuses (`Vellum trusted
    freeze` is published as a status on a pull request's head). A context
    neither carries is `missing`, which is not evidence either way.
    """
    by_status: dict[str, dict] = {}
    for status in statuses:
        context = (status.get("context") or "").strip()
        if context and context not in by_status:
            by_status[context] = status
    results: list[ContextResult] = []
    for context in required:
        owned = [j for j in jobs if (j.get("name") or "").strip() == context]
        if not owned and context == SUITE_CONTEXT:
            owned = [
                j for j in jobs if MACOS_JOB_RE.match((j.get("name") or "").strip())
            ]
        if owned:
            leg = [(j.get("conclusion") or "").strip().lower() for j in owned]
            if "failure" in leg:
                conclusion = "failure"
                pick = owned[leg.index("failure")]
            elif all(c == "success" for c in leg):
                conclusion = "success"
                pick = owned[0]
            else:
                conclusion = next((c for c in leg if c and c != "success"), "")
                pick = owned[0]
            results.append(
                ContextResult(
                    context=context,
                    conclusion=conclusion,
                    source=SOURCE_JOB,
                    execution=_execution(owned) if context == SUITE_CONTEXT else "",
                    run_id=str(pick.get("run_id") or ""),
                )
            )
            continue
        status = by_status.get(context)
        if status is not None:
            state = (status.get("state") or "").strip().lower()
            conclusion = {"success": "success", "failure": "failure", "error": "failure"}.get(
                state, ""
            )
            results.append(
                ContextResult(
                    context=context,
                    conclusion=conclusion,
                    source=SOURCE_STATUS,
                    # A status says nothing about a test step, so a green one
                    # for the suite context cannot prove the suite ran.
                    execution=NOT_EXECUTED if context == SUITE_CONTEXT else "",
                )
            )
            continue
        results.append(ContextResult(context=context, conclusion="", source=SOURCE_MISSING))
    return tuple(results)


def _json(path: str) -> dict:
    raw = gh(path)
    if not raw:
        return {}
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}


def _workflow_path(run: dict) -> str:
    # The runs API can suffix a path with `@ref` for a reusable/dynamic run.
    return (run.get("path") or "").split("@", 1)[0].strip()


def merge_group_runs(
    repo: str, head_sha: str = "", pages: int = 1, before: str = ""
) -> list[dict]:
    """merge_group runs of EVERY workflow, newest first (one call per page).

    `before` (an ISO timestamp) replays the history as it stood then.
    """
    runs: list[dict] = []
    for page in range(1, max(pages, 1) + 1):
        query = f"event=merge_group&per_page=100&page={page}"
        if head_sha:
            query += f"&head_sha={head_sha}"
        if before:
            query += f"&created=<{before}"
        batch = _json(f"repos/{repo}/actions/runs?{query}").get("workflow_runs") or []
        runs.extend(batch)
        if len(batch) < 100:
            break
    return runs


def producing_runs(runs: list[dict], workflows: tuple[str, ...]) -> dict[str, dict]:
    """The newest run of each context-producing workflow among `runs`."""
    wanted = set(workflows)
    newest: dict[str, dict] = {}
    for run in runs:
        path = _workflow_path(run)
        if path not in wanted:
            continue
        held = newest.get(path)
        if held is None or newest_run([held, run]) is run:
            newest[path] = run
    return newest


def heads_newest_first(runs: list[dict]) -> list[tuple[str, list[dict]]]:
    """Group runs by head sha, newest head first (by its newest run)."""
    grouped: dict[str, list[dict]] = {}
    for run in runs:
        head = (run.get("head_sha") or "").strip()
        if head:
            grouped.setdefault(head, []).append(run)
    return sorted(
        grouped.items(),
        key=lambda item: max((r.get("created_at") or "") for r in item[1]),
        reverse=True,
    )


def head_jobs(repo: str, head_sha: str, by_path: dict[str, dict]) -> list[dict]:
    """The jobs that can own a required context on one merge-group head.

    build.yml's jobs come from the jobs API, because `macos` needs its steps to
    tell an executed suite from a receipt reuse. Every other producing workflow
    is read in ONE check-runs call for the commit (an Actions job IS a check
    run), kept only when its check suite belongs to one of those merge_group
    runs -- so a push run on the same landed commit is never read as the gate.
    """
    jobs: list[dict] = []
    build = by_path.get(BUILD_WORKFLOW)
    if build is not None:
        for entry in _jobs(repo, str(build.get("id"))):
            jobs.append({**entry, "run_id": build.get("id"), "workflow": BUILD_WORKFLOW})
    suites = {
        run.get("check_suite_id"): (path, run)
        for path, run in by_path.items()
        if path != BUILD_WORKFLOW and run.get("check_suite_id")
    }
    if suites:
        payload = _json(f"repos/{repo}/commits/{head_sha}/check-runs?per_page=100")
        for check in payload.get("check_runs") or []:
            owner = suites.get((check.get("check_suite") or {}).get("id"))
            if owner is None:
                continue
            path, run = owner
            jobs.append(
                {
                    "name": check.get("name"),
                    "conclusion": check.get("conclusion"),
                    "status": check.get("status"),
                    "id": check.get("id"),
                    "run_id": run.get("id"),
                    "workflow": path,
                }
            )
    return jobs


def commit_statuses(repo: str, sha: str) -> list[dict]:
    """The latest status per context on a commit (combined-status payload)."""
    return _json(f"repos/{repo}/commits/{sha}/status?per_page=100").get("statuses") or []


def log_failing_tests(repo: str, run_id: str, job_name: str) -> tuple[str, ...]:
    """ctest's "The following tests FAILED" block from one job's log.

    For a context that uploads no ctest artifact (`drift-fast` runs ctest over a
    configured tree and publishes nothing), the job log is the only record of
    which tests failed. The RUN log zip is read, because the per-job endpoint is
    withheld by `ghapp` for carrying terminal escapes; the attributor's own
    unpacking and ctest-block parser are reused rather than restated.
    """
    try:
        import queue_batch_attribute as attributor
    except ImportError:
        return ()
    archive = attributor.run_log_zip(repo, run_id)
    if not archive:
        return ()
    text = attributor.unpack_job_logs(archive).get(job_name, "")
    return tuple(dict.fromkeys(attributor.parse_failing_tests(text)))


def context_failing_tests(repo: str, result: ContextResult) -> tuple[str, ...]:
    if not result.failed or not result.run_id:
        return ()
    if result.is_suite:
        return artifact_failing_tests(repo, result.run_id)
    if result.source != SOURCE_JOB:
        return ()
    return log_failing_tests(repo, result.run_id, result.context)


def observe_head(
    repo: str,
    head_sha: str,
    lane: str,
    required: tuple[str, ...],
    workflows: tuple[str, ...],
    runs: list[dict] | None = None,
    source: str = "",
) -> SuiteObservation | None:
    """Every required context on one merge-group head. None when no producing
    workflow ran a merge_group for it."""
    if not head_sha:
        return None
    if runs is None:
        runs = merge_group_runs(repo, head_sha=head_sha)
    by_path = producing_runs(
        [r for r in runs if (r.get("head_sha") or "").strip() == head_sha], workflows
    )
    if not by_path:
        return None
    judged = judge_contexts(
        head_jobs(repo, head_sha, by_path), commit_statuses(repo, head_sha), required
    )
    contexts = tuple(
        ContextResult(
            context=c.context,
            conclusion=c.conclusion,
            source=c.source,
            execution=c.execution,
            run_id=c.run_id,
            failing_tests=context_failing_tests(repo, c),
        )
        for c in judged
    )
    failing = tuple(dict.fromkeys(t for c in contexts if c.failed for t in c.failing_tests))
    primary = by_path.get(BUILD_WORKFLOW) or newest_run(list(by_path.values())) or {}
    first_red = next((c for c in contexts if c.failed), None)
    suite = next((c for c in contexts if c.is_suite), None)
    if first_red is not None:
        conclusion = "failure"
    elif all(c.passed for c in contexts):
        conclusion = "success"
    else:
        conclusion = next(
            (c.conclusion for c in contexts if c.conclusion and c.conclusion != "success"),
            "",
        )
    return SuiteObservation(
        run_id=str((first_red.run_id if first_red and first_red.run_id else primary.get("id")) or ""),
        lane=lane,
        conclusion=conclusion,
        execution=suite.execution if suite is not None else EXECUTED,
        failing_tests=failing,
        base_sha=_base_sha(primary),
        head_sha=head_sha,
        created_at=(primary.get("created_at") or ""),
        source=source,
        contexts=contexts,
    )


def observe_main_by_head_sha(
    repo: str,
    tip_sha: str,
    required: tuple[str, ...] | None = None,
    workflows: tuple[str, ...] | None = None,
) -> SuiteObservation | None:
    """Every required context on the merge group whose head IS main's tip.

    Under the MERGE method the landing commit is the group commit, so its
    merge_group runs tested exactly what is on main. Read at any run status:
    the queue lands a group as soon as its REQUIRED checks pass, while advisory
    legs may still be running. None when no merge_group run exists for the tip.
    """
    return observe_head(
        repo,
        tip_sha,
        "main",
        required or required_contexts(),
        workflows or required_workflows(),
        source=SOURCE_HEAD_SHA,
    )


def observe_main_by_tree(
    repo: str,
    tree: str,
    limit: int = 20,
    required: tuple[str, ...] | None = None,
    workflows: tuple[str, ...] | None = None,
    runs: list[dict] | None = None,
    skip_head: str = "",
) -> SuiteObservation | None:
    """A main observation from an earlier merge group that built the same TREE.

    Covers a tip whose own merge_group run has no executed gate (a revert to a
    tested tree, or a landing commit whose sha differs from the group commit
    while pointing at the same tree): the tree determines what was built.
    """
    if not tree:
        return None
    runs = runs if runs is not None else merge_group_runs(repo)
    for head, head_runs in heads_newest_first(runs)[:limit]:
        if head == skip_head or _tree_sha(repo, head) != tree:
            continue
        seen = observe_head(
            repo,
            head,
            "main",
            required or required_contexts(),
            workflows or required_workflows(),
            runs=head_runs,
            source=SOURCE_TREE_IDENTITY,
        )
        if seen is not None and seen.is_evidence:
            return seen
    return None


def observe_main(
    repo: str,
    limit: int = 20,
    required: tuple[str, ...] | None = None,
    workflows: tuple[str, ...] | None = None,
    tip: tuple[str, str] | None = None,
    runs: list[dict] | None = None,
) -> tuple[SuiteObservation | None, str]:
    """main's health and the tip it is about: the tip's own merge group first,
    tree identity second. A tip observation that is not yet evidence is still
    returned when nothing better exists, so the verdict names what it waits on.
    `tip` pins (sha, tree) for a read-only replay of an earlier main."""
    tip_sha, tree = tip if tip is not None else main_head(repo)
    direct = observe_main_by_head_sha(repo, tip_sha, required, workflows)
    if direct is not None and direct.is_evidence:
        return direct, tip_sha
    return (
        observe_main_by_tree(repo, tree, limit, required, workflows, runs, tip_sha)
        or direct,
        tip_sha,
    )


def observe_batches(
    repo: str,
    limit: int = 12,
    required: tuple[str, ...] | None = None,
    workflows: tuple[str, ...] | None = None,
    runs: list[dict] | None = None,
) -> list[SuiteObservation]:
    """The newest `limit` merge-group heads, each judged on every required context.

    Heads with no completed producing run yet are left out: they cost reads
    and can only be non-evidence.
    """
    required = required or required_contexts()
    workflows = workflows or required_workflows()
    runs = runs if runs is not None else merge_group_runs(repo, pages=2)
    observations: list[SuiteObservation] = []
    for head, head_runs in heads_newest_first(runs):
        if len(observations) >= limit:
            break
        producing = producing_runs(head_runs, workflows)
        if not any((r.get("status") or "") == "completed" for r in producing.values()):
            continue
        seen = observe_head(repo, head, "batch", required, workflows, runs=head_runs)
        if seen is not None:
            observations.append(seen)
    return observations


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


def membership_culprits(
    repo: str, since: str, limit: int
) -> tuple[dict[int, list[str]], str | None]:
    """Queued entries the merge-group history names as breaking a test.

    File ownership cannot see a change that breaks a test it never names; the
    queue's overlapping batch memberships can. Reuses the attributor's rule.
    Returns the culprits and which required set they were judged against.
    """
    try:
        import queue_batch_attribute as attributor
    except ImportError:
        return {}, None
    read = attributor.observe_history(
        repo,
        since=since,
        max_runs=limit,
        cache_dir=attributor.default_cache_dir(repo),
    )
    source = REQUIRED_SOURCE_FALLBACK if read.required_unread else REQUIRED_SOURCE_PROTECTION
    return (
        attributor.likely_culprits(attributor.history_attribution(read.observations)),
        source,
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
        "--tip-sha",
        default="",
        help="judge this commit as main's tip instead of main's current head "
        "(read-only replay of an earlier state)",
    )
    parser.add_argument(
        "--before",
        default="",
        help="read batch history created before this ISO timestamp (replay)",
    )
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

    required = required_contexts()
    workflows = required_workflows()
    tip = None
    history = None
    if args.tip_sha:
        tip = (args.tip_sha, _tree_sha(args.repo, args.tip_sha))
    if args.before:
        history = merge_group_runs(args.repo, pages=2, before=args.before)
    main_observation, tip_sha = observe_main(
        args.repo, args.main_limit, required, workflows, tip=tip, runs=history
    )
    verdict = detect(
        main_observation,
        observe_batches(args.repo, args.batch_limit, required, workflows, runs=history),
        min_streak=args.min_streak,
        tip_sha=tip_sha,
    )
    fix = candidate_fix_pr(args.repo, verdict.tests) if args.name_fix_pr else None
    culprits, source = (
        membership_culprits(args.repo, args.history_since, args.history_limit)
        if args.name_fix_pr
        else (None, None)
    )
    payload = signal(verdict, fix, culprits, source)
    _emit(payload, args.output or None)
    if args.fail_on_poisoned and payload["safe_to_pause_queue"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
