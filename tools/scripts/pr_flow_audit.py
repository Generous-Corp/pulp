#!/usr/bin/env python3
"""Classify every open PR as moving, auto-fixable, waiting on a human, or unknown.

Why this exists
---------------
CI is reactive: it runs when a pull request is pushed or enqueued. Nothing ever
asks "is anything stuck that should not be?", so a backlog can stop draining
while the fleet sits idle. A 38-PR backlog drained to 16 and then stalled with
every runner free, because the remaining PRs were each blocked on something that
generates no event and therefore never self-heals.

The governing rule
------------------
A PR must either have work in flight, or be waiting on a NAMED human decision.
Anything else is a leak.

Every open PR lands in exactly one bucket:

  MOVING        a check is queued or running, or the PR is in the merge queue.
  AUTO-FIXABLE  a mechanical action returns it to MOVING; --fix performs it.
  NEEDS-HUMAN   a named decision is required, and the name is printed.
  UNKNOWN       the state could not be determined. Never auto-fixed.

Why the merge-queue watchdog does not already cover this
--------------------------------------------------------
`merge_stall_watchdog.py` answers a narrower question — "are green, armed,
mergeable PRs failing to land?" — and to do so it restricts itself to
mergeStateStatus in {CLEAN, BEHIND} and `continue`s past DIRTY, BLOCKED and
UNSTABLE. Those skipped states are exactly where the leaks were found. This tool
covers the complement: it never reports health, only where flow has stopped.

Reachability is evaluated, never hardcoded
------------------------------------------
A PR can fail at a step that no longer runs. Naming that step in a constant
would rot at the next workflow edit, which is the failure this tool exists to
catch. Instead the workflow is parsed and each failing step's `if:` expression
is evaluated against the `pull_request` event.

The refresh action is update-branch, not re-run
-----------------------------------------------
Re-running a workflow replays the workflow file from the run's own commit. If a
step failed, that run's workflow reached it, so a re-run reaches it again — a
re-run can never clear a stale-gate failure. Updating the branch recomputes the
merge commit against current base, which produces a fresh run under the current
workflow, where the step is skipped. Measured directly: an affected PR's
`refs/pull/N/merge` still carried the pre-change workflow hours after base had
moved on, while the PR touched no workflow file at all.

Queue membership is read, never inferred
---------------------------------------
GitHub CONSUMES a pull request's auto-merge request when the queue takes it, so
a queued PR reports `auto_merge: null` — the same value as one that was never
armed. Membership therefore comes from GraphQL (`isInMergeQueue`,
`mergeQueueEntry`), and two further states that are also pure absences in REST
are read from the timeline: a PR the queue EJECTED, which returns to `clean`
with a null auto-merge request, and one a human deliberately disarmed. Re-arming
either is a mutation that undoes somebody's decision — and under ALLGREEN
grouping, re-enqueueing a known-bad PR fails the innocent PRs batched with it.
A queued PR also reports its mergeable state as UNKNOWN for as long as it sits
there, so membership is settled before state is consulted.

Fail closed
-----------
A false "nothing to do" is the failure mode being eliminated, so it must be
impossible to emit. Any PR whose state cannot be established — API error, absent
checks, unparseable or ambiguous workflow condition, an unreadable queue
membership — is reported UNKNOWN with the reason, and `--fix` refuses to touch
it. A condition that depends on the outcome of its own run (`!cancelled()`) is
undecidable rather than false, because false is the direction that produces a
mutation.

Mutating is opt-in and capped
-----------------------------
`--fix` reports what it would do; `--apply` is what performs it, and
`--max-fixes` bounds how many. A classifier bug does not mislabel one PR, it
mislabels a shape of PR, so the blast radius of a wrong verdict is the whole
backlog unless something bounds it. A fix that FAILED exits non-zero; findings
alone never do.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

# The workflow that owns the required gate, and the event a pull request runs
# under. Both are inputs to reachability, not assertions about any step.
DEFAULT_WORKFLOW = ".github/workflows/build.yml"
PULL_REQUEST_EVENT = "pull_request"
DEFAULT_GATE = "macos"
DEFAULT_UNSTABLE_HOURS = 6.0
# How long a check may sit pending before it stops counting as progress. The
# required macOS gate is served by a small pool and queues for hours by design,
# so this is deliberately generous: it catches a wedge, not a queue.
DEFAULT_IN_FLIGHT_HOURS = 24.0
# The most mutations one sweep may perform. A classifier bug that mislabels a
# whole backlog then costs three actions, not forty.
DEFAULT_MAX_FIXES = 3

# Conclusions that leave a check with nothing outstanding.
GREEN = {"success", "skipped", "neutral"}

# A job conclusion that is not a rejection of the change. `cancelled` in
# particular is somebody freeing the one required-gate runner on purpose;
# replaying it undoes their decision.
NOT_A_REJECTION = {"cancelled", "skipped", "stale", "action_required", "timed_out", "neutral"}

STATUS_FUNCTIONS = ("success", "failure", "cancelled", "always")

# The two outcome assignments every condition is evaluated under. A step that
# is reachable in EITHER can still execute. Pinning every status function to
# True instead makes `!cancelled()` — the standard "run even after a failure"
# idiom — evaluate False, which reports a step that genuinely failed as one a
# fresh run would never reach, and that verdict points at `update-branch`.
_OUTCOME_ASSIGNMENTS = (
    {"success": True, "failure": False, "cancelled": False, "always": True},
    {"success": False, "failure": True, "cancelled": False, "always": True},
    {"success": False, "failure": False, "cancelled": True, "always": True},
)
PENDING_STATUS = {"queued", "in_progress", "pending", "waiting", "requested"}

# Bucket names, used as both report headings and JSON values.
MOVING = "MOVING"
AUTO_FIXABLE = "AUTO-FIXABLE"
NEEDS_HUMAN = "NEEDS-HUMAN"
UNKNOWN = "UNKNOWN"

# Ordering for the report: cheapest human wins first, then everything else that
# has stopped, then what is already fine.
BUCKET_ORDER = [NEEDS_HUMAN, AUTO_FIXABLE, UNKNOWN, MOVING]


class WorkflowError(Exception):
    """A workflow condition could not be resolved, so reachability is unknown."""


# --------------------------------------------------------------------------
# GitHub expression evaluation
# --------------------------------------------------------------------------
#
# A deliberately small recursive-descent evaluator over the subset of GitHub's
# expression syntax that step conditions use: literals, context lookups,
# ==/!=, &&/||, ! and parentheses, plus the status functions. Anything outside
# that subset raises WorkflowError rather than guessing, because a wrong answer
# here silently reclassifies a real failure as auto-fixable.

_TOKEN = re.compile(
    r"""\s*(?:
        (?P<op>&&|\|\||==|!=|\(|\)|!)
      | (?P<str>'(?:[^']|'')*')
      | (?P<name>[A-Za-z_][A-Za-z0-9_.\-]*)
    )""",
    re.VERBOSE,
)


def _tokenize(expr: str) -> list[tuple[str, str]]:
    tokens: list[tuple[str, str]] = []
    pos = 0
    while pos < len(expr):
        if expr[pos].isspace():
            pos += 1
            continue
        match = _TOKEN.match(expr, pos)
        if not match or match.end() == pos:
            raise WorkflowError(f"unparseable token at offset {pos} in {expr!r}")
        pos = match.end()
        for kind in ("op", "str", "name"):
            value = match.group(kind)
            if value is not None:
                tokens.append((kind, value))
                break
    return tokens


class _Parser:
    """Parses and evaluates in one pass; context is fixed for the whole expression."""

    def __init__(self, tokens: list[tuple[str, str]], context: dict[str, Any]) -> None:
        self.tokens = tokens
        self.pos = 0
        self.context = context

    def peek(self) -> tuple[str, str] | None:
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None

    def take(self) -> tuple[str, str]:
        token = self.peek()
        if token is None:
            raise WorkflowError("expression ended early")
        self.pos += 1
        return token

    def parse(self) -> Any:
        value = self.parse_or()
        if self.peek() is not None:
            raise WorkflowError(f"trailing tokens in expression: {self.tokens[self.pos:]}")
        return value

    def parse_or(self) -> Any:
        left = self.parse_and()
        while self.peek() == ("op", "||"):
            self.take()
            right = self.parse_and()
            # GitHub's || returns the first truthy operand; only truthiness is
            # consumed here, so collapsing to a bool is faithful enough.
            left = _truthy(left) or _truthy(right)
        return left

    def parse_and(self) -> Any:
        left = self.parse_unary()
        while self.peek() == ("op", "&&"):
            self.take()
            right = self.parse_unary()
            left = _truthy(left) and _truthy(right)
        return left

    def parse_unary(self) -> Any:
        if self.peek() == ("op", "!"):
            self.take()
            start = self.pos
            value = self.parse_unary()
            if self._spans_status_function(start, self.pos):
                # Whether the negation holds depends on the outcome of the very
                # run the step sits in, so no fixed assignment answers it. The
                # dangerous direction is a False here: it reads as "a fresh run
                # never reaches this step", which points --fix at update-branch
                # for a step that genuinely failed.
                raise WorkflowError(
                    "negated status function: reachability depends on the run's "
                    "own outcome and cannot be decided statically"
                )
            return not _truthy(value)
        return self.parse_comparison()

    def _spans_status_function(self, start: int, end: int) -> bool:
        return any(
            kind == "name" and value in STATUS_FUNCTIONS
            for kind, value in self.tokens[start:end]
        )

    def parse_comparison(self) -> Any:
        left = self.parse_primary()
        token = self.peek()
        if token is not None and token[0] == "op" and token[1] in ("==", "!="):
            self.take()
            right = self.parse_primary()
            return left == right if token[1] == "==" else left != right
        return left

    def parse_primary(self) -> Any:
        kind, value = self.take()
        if kind == "op" and value == "(":
            inner = self.parse_or()
            if self.take() != ("op", ")"):
                raise WorkflowError("unbalanced parenthesis")
            return inner
        if kind == "str":
            return value[1:-1].replace("''", "'")
        if kind == "name":
            # A status function: success(), failure(), cancelled(), always().
            if self.peek() == ("op", "("):
                self.take()
                if self.take() != ("op", ")"):
                    raise WorkflowError(f"{value}() takes no arguments")
                if value not in self.context["functions"]:
                    raise WorkflowError(f"unsupported function {value}()")
                return self.context["functions"][value]
            lowered = value.lower()
            if lowered in ("true", "false"):
                return lowered == "true"
            if value in self.context["values"]:
                return self.context["values"][value]
            raise WorkflowError(f"unknown context reference {value!r}")
        raise WorkflowError(f"unexpected token {value!r}")


def _truthy(value: Any) -> bool:
    return bool(value)


def _strip_wrappers(expr: str) -> str:
    """Remove the optional ${{ }} wrapper a condition may carry."""
    text = str(expr).strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    return text


def evaluate_condition(expr: str | bool | None, *, event_name: str, runner_os: str) -> bool:
    """Evaluate a step `if:` for one event and runner OS.

    An absent condition means the step always runs. The question is "could this
    step execute under this event", not "did it", so the condition is evaluated
    once per outcome assignment (success path and failure path) and the step is
    reachable when either says so. A negated status function is decided by
    neither and raises WorkflowError rather than resolving to a guess.
    """
    if expr is None:
        return True
    if isinstance(expr, bool):
        return expr
    text = _strip_wrappers(expr)
    if not text:
        return True
    tokens = _tokenize(text)
    values = {"github.event_name": event_name, "runner.os": runner_os}
    for functions in _OUTCOME_ASSIGNMENTS:
        context = {"values": values, "functions": dict(functions)}
        if _truthy(_Parser(list(tokens), context).parse()):
            return True
    return False


def is_consequence_condition(expr: str | bool | None) -> bool:
    """True when a step only runs because an earlier step already failed.

    Such a step is a reporting consequence, never an independent cause: if the
    real failure is cleared, this one cannot fire. Counting it as a blocker
    would keep a recoverable PR pinned to NEEDS-HUMAN forever.
    """
    if not isinstance(expr, str):
        return False
    text = _strip_wrappers(expr)
    return bool(re.search(r"\b(failure|cancelled)\s*\(\s*\)", text))


# --------------------------------------------------------------------------
# Workflow model
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class StepCondition:
    """A step's declared condition, as written in the workflow.

    `ambiguous` marks a name that two jobs use under different conditions. The
    ambiguity is recorded per step rather than raised for the whole workflow,
    because a name collision somewhere else says nothing about the step that
    actually failed — failing the entire parse would manufacture UNKNOWN
    verdicts for PRs whose own failure is perfectly decidable.
    """

    name: str
    expr: str | bool | None
    ambiguous: bool = False


def _load_workflow(text: str) -> Any:
    """Parse a workflow document, or say why reachability is undecidable.

    PyYAML is imported here rather than at module scope because stock CI
    runners do not carry it, and this module is also loaded by a ctest that
    must run on those runners. Its absence is not a crash and not a default:
    it fails closed, exactly like an unparseable condition, so every verdict
    that needed the workflow degrades to UNKNOWN.
    """
    try:
        import yaml
    except ImportError as exc:
        raise WorkflowError(
            "PyYAML is not installed, so workflow reachability cannot be "
            "evaluated"
        ) from exc
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise WorkflowError(f"workflow is not valid YAML: {exc}") from exc


def parse_workflow_steps(text: str) -> dict[str, StepCondition]:
    """Map step name -> condition for every named step in a workflow."""
    document = _load_workflow(text)
    if not isinstance(document, dict):
        raise WorkflowError("workflow root is not a mapping")
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        raise WorkflowError("workflow declares no jobs mapping")

    seen: dict[str, list[Any]] = {}
    for job in jobs.values():
        if not isinstance(job, dict):
            continue
        for step in job.get("steps") or []:
            if not isinstance(step, dict):
                continue
            name = step.get("name")
            if not isinstance(name, str):
                continue
            seen.setdefault(name, []).append(step.get("if"))

    steps: dict[str, StepCondition] = {}
    for name, conditions in seen.items():
        distinct = {_strip_wrappers(c) if isinstance(c, str) else c for c in conditions}
        steps[name] = StepCondition(
            name=name, expr=conditions[0], ambiguous=len(distinct) > 1
        )
    return steps


@dataclass(frozen=True)
class StepVerdict:
    """Why one failing step does or does not still block the PR.

    `decidable` is false when the base workflow cannot answer the question at
    all. Such a step neither clears a PR nor condemns it; it forces UNKNOWN
    unless some other step is definitively blocking.
    """

    name: str
    blocking: bool
    reason: str
    decidable: bool = True


def classify_failing_step(
    name: str, steps: dict[str, StepCondition], *, runner_os: str = "macOS"
) -> StepVerdict:
    """Decide whether a failing step would block a fresh pull_request run.

    `runner_os` is the OS of the job the step actually ran in, not the gate's
    nominal platform: a condition like `runner.os != 'Windows'` answers
    differently per job, so judging a Linux step under macOS inverts it.
    """
    condition = steps.get(name)
    if condition is None:
        # Absent from the base workflow means either that base removed the step
        # or that this PR introduces it. Those imply opposite verdicts — a
        # removed step cannot fail again, a newly added one certainly can — and
        # absence alone cannot tell them apart.
        return StepVerdict(
            name,
            False,
            "absent from the base workflow: either removed by base or added by "
            "this PR, and absence cannot distinguish them",
            decidable=False,
        )
    if condition.ambiguous:
        return StepVerdict(
            name,
            False,
            "appears under more than one condition, so the one that failed "
            "cannot be identified",
            decidable=False,
        )
    if not evaluate_condition(
        condition.expr, event_name=PULL_REQUEST_EVENT, runner_os=runner_os
    ):
        return StepVerdict(
            name, False, f"not reachable for {PULL_REQUEST_EVENT}: if: {condition.expr}"
        )
    if is_consequence_condition(condition.expr):
        return StepVerdict(
            name, False, "reports an earlier failure rather than causing one"
        )
    return StepVerdict(name, True, "still runs and still fails")


# --------------------------------------------------------------------------
# PR classification
# --------------------------------------------------------------------------


@dataclass
class Finding:
    """One PR's verdict, with the evidence that produced it."""

    number: int
    title: str
    bucket: str
    reason: str
    action: str = ""
    cheap_win: bool = False
    detail: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> dict[str, Any]:
        return {
            "number": self.number,
            "title": self.title,
            "bucket": self.bucket,
            "reason": self.reason,
            "action": self.action,
            "cheap_win": self.cheap_win,
            "detail": self.detail,
        }


def _gate_check(pr: dict[str, Any], gate: str) -> dict[str, Any] | None:
    """The most recent check run for the required gate, if one exists."""
    runs = [c for c in pr.get("checks") or [] if c.get("name") == gate]
    if not runs:
        return None
    return sorted(runs, key=lambda c: str(c.get("started_at") or ""))[-1]


def _pending_checks(pr: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        c
        for c in pr.get("checks") or []
        if str(c.get("status") or "").lower() in PENDING_STATUS
    ]


def _work_in_flight(
    pr: dict[str, Any],
    required: list[str],
    now: datetime,
    in_flight_hours: float,
) -> tuple[bool, list[dict[str, Any]]]:
    """Is real work running, and which pending checks are no longer evidence of it?

    "Something is pending" is not the same as "this PR is moving". A stuck
    advisory lane pends forever and would mark every wedged PR MOVING, which is
    precisely the silence this tool exists to break. Two narrowings, in order of
    strength: when the required contexts are known, only a pending REQUIRED
    context counts; and in either case a check pending longer than the flight
    window has stopped being evidence of progress.
    """
    pending = _pending_checks(pr)
    if not pending:
        return False, []
    if required:
        pending = [c for c in pending if str(c.get("name") or "") in required]
        if not pending:
            return False, []
    fresh, stale = [], []
    for check in pending:
        age = _hours_since(check.get("started_at"), now)
        (stale if age is not None and age >= in_flight_hours else fresh).append(check)
    return bool(fresh), stale


def _hours_since(value: str | None, now: datetime) -> float | None:
    if not value:
        return None
    try:
        stamp = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=timezone.utc)
    return (now - stamp).total_seconds() / 3600.0


def classify_pr(
    pr: dict[str, Any],
    steps: dict[str, StepCondition] | None,
    workflow_error: str,
    now: datetime,
    *,
    gate: str = DEFAULT_GATE,
    unstable_hours: float = DEFAULT_UNSTABLE_HOURS,
    in_flight_hours: float = DEFAULT_IN_FLIGHT_HOURS,
    required: list[str] | None = None,
) -> Finding:
    """Place one PR in exactly one bucket.

    `steps` is None when the workflow could not be parsed; every verdict that
    would depend on it then degrades to UNKNOWN rather than to a default.
    """
    number = pr.get("number")
    title = str(pr.get("title") or "")
    base = Finding(number=int(number or 0), title=title, bucket=UNKNOWN, reason="")

    if not isinstance(number, int):
        base.reason = "pull request has no number"
        return base

    # Anything the collector could not read makes every downstream verdict a
    # guess. Report the gap instead of a bucket.
    if pr.get("errors"):
        base.reason = "collection error: " + "; ".join(str(e) for e in pr["errors"])
        return base

    if pr.get("in_merge_queue") is None:
        # Queue membership is not inferable from `auto_merge`: GitHub CONSUMES
        # the auto-merge request on enqueue, so a queued PR reports
        # auto_merge=null and is indistinguishable from one never armed. A
        # collector that did not read it must not let the arming branch run.
        base.reason = "merge-queue membership could not be read"
        return base

    if pr.get("in_merge_queue"):
        base.bucket = MOVING
        base.reason = "in the merge queue" + (
            f" ({pr['merge_queue_state']})" if pr.get("merge_queue_state") else ""
        )
        return base

    state = str(pr.get("mergeable_state") or "").lower()
    if not state or state == "unknown":
        base.reason = "GitHub has not computed a mergeable state yet"
        return base

    if pr.get("draft"):
        base.bucket = NEEDS_HUMAN
        base.reason = "draft — waiting on its author to mark it ready"
        base.action = "author marks ready for review"
        return base

    # An ejected PR returns to `clean` with a null auto-merge request, which
    # reads identically to "never armed". Re-arming it re-enqueues the same
    # failure every sweep, each cycle consuming a queue slot and, under
    # ALLGREEN grouping, failing the innocent PRs batched with it.
    ejected = pr.get("ejected_from_queue")
    if ejected:
        base.bucket = NEEDS_HUMAN
        base.reason = (
            "the merge queue ejected it"
            + (f" at {ejected.get('at')}" if ejected.get("at") else "")
            + " — re-arming would re-enqueue the same failure"
        )
        base.action = "find why the batch failed before re-arming"
        base.detail = {"ejected_from_queue": ejected}
        return base

    batch = pr.get("merge_group_failure")
    if batch:
        base.bucket = NEEDS_HUMAN
        base.reason = (
            f"its merge-queue batch failed (run {batch.get('run_id')}) and it is "
            "no longer in the queue"
        )
        base.action = "attribute the batch failure before re-arming"
        base.detail = {"merge_group_failure": batch}
        return base

    checks = pr.get("checks")
    if checks is None:
        base.reason = "check runs could not be read"
        return base

    moving, stale_pending = _work_in_flight(pr, required or [], now, in_flight_hours)
    if moving:
        base.bucket = MOVING
        base.reason = "a check is queued or running"
        return base
    if stale_pending:
        names = ", ".join(str(c.get("name")) for c in stale_pending)
        base.bucket = NEEDS_HUMAN
        base.reason = (
            f"pending for over {in_flight_hours:g}h with no progress: {names}"
        )
        base.action = "cancel or re-dispatch the wedged check"
        base.detail = {"stale_pending": [str(c.get("name")) for c in stale_pending]}
        return base

    gate_run = _gate_check(pr, gate)
    gate_conclusion = str((gate_run or {}).get("conclusion") or "").lower()
    gate_green = gate_conclusion in GREEN

    # Conflicts come first: they are the cheapest human wins in any backlog,
    # and a conflicted PR cannot be mechanically advanced at all.
    if state == "dirty":
        base.bucket = NEEDS_HUMAN
        base.cheap_win = gate_green
        if gate_green:
            base.reason = (
                f"conflicted, but the {gate} gate is already green — "
                "only the conflict stands between it and the queue"
            )
        else:
            base.reason = f"conflicted; the {gate} gate is {gate_conclusion or 'absent'}"
        base.action = "resolve the conflict"
        base.detail = {"mergeable_state": state, "gate_conclusion": gate_conclusion}
        return base

    # A required gate that never registered produces no event and cannot
    # self-heal, so it is a leak even though nothing is red.
    if gate_run is None:
        base.bucket = NEEDS_HUMAN
        base.reason = f"the required {gate} gate never registered on this head"
        base.action = f"dispatch the workflow for this branch, or push to re-trigger {gate}"
        base.detail = {"mergeable_state": state}
        return base

    if gate_conclusion and gate_conclusion not in GREEN:
        if gate_conclusion in NOT_A_REJECTION:
            # Somebody stopped this run, most often to free the single
            # required-gate runner. Re-dispatching it reverses their decision
            # and takes the runner straight back.
            base.bucket = NEEDS_HUMAN
            base.reason = (
                f"the {gate} gate was {gate_conclusion} rather than failing — "
                "replaying it would undo whoever stopped it"
            )
            base.action = f"decide whether {gate} should be re-dispatched"
            base.detail = {"gate_conclusion": gate_conclusion}
            return base
        failed_steps = pr.get("gate_failed_steps")
        if failed_steps is None:
            base.reason = f"{gate} failed but its steps could not be read"
            return base
        if not failed_steps:
            # "No failing step" only means the run was LOST when the job really
            # did fail. A cancelled job also reports zero failing steps — it
            # carries none at all — and replaying that is the mutation this
            # branch must never make, so the job's own conclusion is required.
            job_conclusion = str(pr.get("gate_job_conclusion") or "").lower()
            if job_conclusion != "failure":
                base.reason = (
                    f"{gate} is {gate_conclusion} with no failing step, and its "
                    f"job conclusion is {job_conclusion or 'unknown'} — a lost run "
                    "cannot be distinguished from a stopped one"
                )
                base.detail = {"gate_job_conclusion": job_conclusion}
                return base
            base.bucket = AUTO_FIXABLE
            base.reason = f"{gate} failed with no failing step — the run was lost"
            base.action = "rerun-failed-jobs"
            base.detail = {"run_id": gate_run.get("run_id")}
            return base
        # Only now does the workflow matter: every remaining verdict asks
        # whether a fresh run would still reach a named step.
        if steps is None:
            base.reason = (
                f"{gate} failed, and reachability is undecidable: {workflow_error}"
            )
            return base
        runner_os = str(pr.get("gate_runner_os") or "macOS")
        try:
            verdicts = [
                classify_failing_step(s, steps, runner_os=runner_os)
                for s in failed_steps
            ]
        except WorkflowError as exc:
            base.reason = f"{gate} failed, and reachability is undecidable: {exc}"
            return base
        # A definitively blocking step settles the PR whatever else is unclear:
        # it needs a human either way. Only when nothing blocks does an
        # undecidable step matter, and then it must not be read as "clear".
        blocking = [v for v in verdicts if v.blocking]
        if blocking:
            base.bucket = NEEDS_HUMAN
            names = ", ".join(v.name for v in blocking)
            base.reason = f"{gate} failed at a step that still runs: {names}"
            base.action = "fix the failing step"
            base.detail = {"blocking_steps": [v.name for v in blocking]}
            return base
        undecidable = [v for v in verdicts if not v.decidable]
        if undecidable:
            base.reason = f"{gate} failed, and reachability is undecidable: " + "; ".join(
                f"{v.name} ({v.reason})" for v in undecidable
            )
            return base
        # Every failing step is unreachable under the current workflow.
        # A re-run would replay the old workflow and fail identically; only a
        # fresh merge commit picks up the current one.
        base.bucket = AUTO_FIXABLE
        base.reason = (
            f"{gate} failed only at steps a fresh run would not reach: "
            + "; ".join(f"{v.name} ({v.reason})" for v in verdicts)
        )
        base.action = "update-branch"
        base.detail = {"stale_steps": [v.name for v in verdicts]}
        return base

    # The gate is green. What is left is a state problem, not a test problem.
    armed = bool(pr.get("auto_merge"))
    if state in ("clean", "behind") and not armed:
        # "Never armed" and "a human turned it off" are the same absence in the
        # API. Only the timeline tells them apart, and the distinction is the
        # whole decision here: with required_approving_review_count=0, arming
        # IS merging, so re-arming a PR somebody deliberately disarmed merges
        # work they were holding back.
        disabled_by = pr.get("auto_merge_disabled_by")
        if disabled_by:
            base.bucket = NEEDS_HUMAN
            base.reason = (
                f"mergeable ({state}) but {disabled_by} turned auto-merge off — "
                "re-arming it would merge work somebody was holding"
            )
            base.action = f"ask {disabled_by} whether it should merge"
            base.detail = {"auto_merge_disabled_by": disabled_by}
            return base
        base.bucket = AUTO_FIXABLE
        base.reason = f"mergeable ({state}) but auto-merge was never armed"
        base.action = "enable-auto-merge"
        base.detail = {"mergeable_state": state}
        return base

    if state == "behind":
        base.bucket = MOVING
        base.reason = "behind base, armed — the queue updates it"
        return base

    if state == "unstable":
        red = [
            c.get("name")
            for c in checks
            if str(c.get("conclusion") or "").lower() not in GREEN
            and str(c.get("status") or "").lower() == "completed"
        ]
        # updated_at is bumped by any comment, including a bot's, so a chatty
        # PR could never accrue the threshold. The last CI activity is what the
        # window is actually about.
        age = _hours_since(pr.get("last_ci_at") or pr.get("updated_at"), now)
        if age is not None and age >= unstable_hours:
            base.bucket = NEEDS_HUMAN
            base.reason = (
                f"unstable for {age:.1f}h with nothing running; "
                f"failing: {', '.join(str(r) for r in red) or 'unattributed'}"
            )
            base.action = "triage the failing check"
            base.detail = {"failing_checks": red, "hours": round(age, 1)}
            return base
        if age is None:
            base.reason = "unstable, and its age could not be determined"
            return base
        base.bucket = NEEDS_HUMAN
        base.reason = (
            f"unstable for {age:.1f}h; failing: "
            f"{', '.join(str(r) for r in red) or 'unattributed'}"
        )
        base.action = "triage the failing check"
        base.detail = {"failing_checks": red, "hours": round(age, 1)}
        return base

    if state == "blocked":
        base.bucket = NEEDS_HUMAN
        base.reason = "blocked — a required review or context is outstanding"
        base.action = "obtain the outstanding review or context"
        return base

    if state == "clean" and armed:
        base.bucket = MOVING
        base.reason = "clean and armed — the queue will take it"
        return base

    base.reason = f"unhandled mergeable state {state!r}"
    return base


def analyze(snapshot: dict[str, Any], now: datetime, **kwargs: Any) -> list[Finding]:
    """Classify every PR in a snapshot. Pure: no network, no clock, no mutation."""
    steps: dict[str, StepCondition] | None
    workflow_error = ""
    workflow_text = snapshot.get("workflow_text")
    if workflow_text is None:
        steps = None
        workflow_error = snapshot.get("workflow_error") or "workflow was not collected"
    else:
        try:
            steps = parse_workflow_steps(workflow_text)
        except WorkflowError as exc:
            steps = None
            workflow_error = str(exc)
    kwargs.setdefault("required", snapshot.get("required_contexts") or [])
    findings = [
        classify_pr(pr, steps, workflow_error, now, **kwargs)
        for pr in snapshot.get("prs") or []
    ]
    return sorted(findings, key=lambda f: (not f.cheap_win, f.number))


# --------------------------------------------------------------------------
# Collection
# --------------------------------------------------------------------------


def _gh(gh_bin: str, *args: str) -> str:
    proc = subprocess.run(
        [gh_bin, *args], capture_output=True, text=True, check=True
    )
    return proc.stdout


def _gh_json(gh_bin: str, path: str) -> Any:
    return json.loads(_gh(gh_bin, "api", "-H", "Accept: application/vnd.github+json", path))


_PR_QUERY = """
query($owner:String!,$name:String!,$base:String!,$cursor:String){
  repository(owner:$owner,name:$name){
    pullRequests(states:OPEN,first:50,baseRefName:$base,after:$cursor){
      pageInfo{hasNextPage endCursor}
      nodes{
        number title isDraft updatedAt mergeStateStatus
        isInMergeQueue
        mergeQueueEntry{state position}
        autoMergeRequest{mergeMethod}
        headRefOid
      }
    }
  }
}
"""

# Timeline events that decide whether an absent auto-merge request is an
# omission or a decision, and whether the queue threw this PR out.
_QUEUE_EVENTS = ("added_to_merge_queue", "removed_from_merge_queue")
_ARM_EVENTS = ("auto_merge_enabled", "auto_merge_disabled")


def _graphql(gh_bin: str, query: str, **variables: Any) -> Any:
    args = ["api", "graphql", "-f", f"query={query}"]
    for key, value in variables.items():
        if value is None:
            continue
        args += ["-f", f"{key}={value}"]
    return json.loads(_gh(gh_bin, *args))


def _paginate(gh_bin: str, path: str, key: str | None = None) -> tuple[list[Any], int | None]:
    """Read every page of a REST listing.

    Returns the items and, when the endpoint reports one, its `total_count`, so
    the caller can prove nothing was silently dropped. A single per_page=100
    read looks complete at exactly the point it stops being complete.
    """
    items: list[Any] = []
    total: int | None = None
    page = 1
    sep = "&" if "?" in path else "?"
    while True:
        payload = _gh_json(gh_bin, f"{path}{sep}per_page=100&page={page}")
        if key is None:
            batch = payload
        else:
            batch = payload.get(key) or []
            if total is None and isinstance(payload.get("total_count"), int):
                total = payload["total_count"]
        if not isinstance(batch, list):
            raise ValueError(f"{path}: expected a list, got {type(batch).__name__}")
        items.extend(batch)
        if len(batch) < 100:
            return items, total
        page += 1
        if page > 20:
            raise ValueError(f"{path}: refusing to page past 2000 items")


def _required_contexts(gh_bin: str, repo: str, base: str) -> list[str]:
    """The contexts branch protection or an active ruleset actually requires.

    Empty is a legitimate answer (and is what classic protection returns for a
    ruleset-governed branch), so callers must treat it as "unknown", never as
    "nothing is required".
    """
    names: list[str] = []
    try:
        checks = _gh_json(
            gh_bin, f"repos/{repo}/branches/{base}/protection/required_status_checks"
        )
        names += [str(c) for c in checks.get("contexts") or []]
    except (subprocess.CalledProcessError, json.JSONDecodeError, AttributeError):
        pass
    try:
        for ruleset in _gh_json(gh_bin, f"repos/{repo}/rulesets") or []:
            if str(ruleset.get("enforcement")) != "active":
                continue
            detail = _gh_json(gh_bin, f"repos/{repo}/rulesets/{ruleset.get('id')}")
            for rule in detail.get("rules") or []:
                if rule.get("type") != "required_status_checks":
                    continue
                params = rule.get("parameters") or {}
                names += [
                    str(c.get("context"))
                    for c in params.get("required_status_checks") or []
                    if c.get("context")
                ]
    except (subprocess.CalledProcessError, json.JSONDecodeError, AttributeError, TypeError):
        pass
    return sorted(set(names))


def _merge_group_failures(gh_bin: str, repo: str) -> dict[int, dict[str, Any]]:
    """Latest merge_group run outcome per PR number, read from its queue branch.

    A queue branch is `gh-readonly-queue/<base>/pr-<N>-<sha>`, so one listing
    attributes every recent batch outcome to the PRs that were in it.
    """
    payload = _gh_json(
        gh_bin,
        f"repos/{repo}/actions/runs?event=merge_group&status=completed&per_page=100",
    )
    runs = payload.get("workflow_runs") or []
    latest: dict[int, dict[str, Any]] = {}
    for run in runs:
        branch = str(run.get("head_branch") or "")
        created = str(run.get("created_at") or "")
        for match in re.finditer(r"pr-(\d+)-", branch):
            number = int(match.group(1))
            if created >= str(latest.get(number, {}).get("at") or ""):
                latest[number] = {
                    "at": created,
                    "run_id": run.get("id"),
                    "conclusion": str(run.get("conclusion") or "").lower(),
                    "head_branch": branch,
                }
    return latest


def _timeline_signals(gh_bin: str, repo: str, number: int) -> dict[str, Any]:
    """Queue ejection and deliberate disarming, read from the PR timeline.

    Both are absences in the REST payload — an ejected PR and a disarmed one
    both report `auto_merge: null` — so only the ordered event history can tell
    either apart from "nothing ever happened".
    """
    events, _ = _paginate(gh_bin, f"repos/{repo}/issues/{number}/timeline")
    last_queue: dict[str, Any] | None = None
    last_arm: dict[str, Any] | None = None
    for event in events:
        if not isinstance(event, dict):
            continue
        kind = str(event.get("event") or "")
        if kind in _QUEUE_EVENTS:
            last_queue = event
        elif kind in _ARM_EVENTS:
            last_arm = event
    signals: dict[str, Any] = {"ejected_from_queue": None, "auto_merge_disabled_by": None}
    if last_queue is not None and str(last_queue.get("event")) == "removed_from_merge_queue":
        signals["ejected_from_queue"] = {
            "at": last_queue.get("created_at"),
            "actor": ((last_queue.get("actor") or {}).get("login")),
        }
    if last_arm is not None and str(last_arm.get("event")) == "auto_merge_disabled":
        actor = (last_arm.get("actor") or {}).get("login")
        # A bot disabling auto-merge is the mechanism doing its job (the queue
        # consumes the request on enqueue); only a person disabling it is a
        # decision this tool must not overturn.
        if actor and str((last_arm.get("actor") or {}).get("type") or "").lower() != "bot":
            signals["auto_merge_disabled_by"] = str(actor)
    return signals


def _runner_os(job: dict[str, Any]) -> str:
    """The OS a job actually ran on, from its runs-on labels or its name."""
    haystack = " ".join(
        [str(job.get("name") or ""), *[str(l) for l in job.get("labels") or []]]
    ).lower()
    if "windows" in haystack:
        return "Windows"
    if "ubuntu" in haystack or "linux" in haystack:
        return "Linux"
    return "macOS"


def collect(
    repo: str,
    gh_bin: str,
    gate: str,
    workflow_path: str,
    base: str = "main",
) -> dict[str, Any]:
    """Build a snapshot. Every read that fails is recorded, never defaulted."""
    snapshot: dict[str, Any] = {
        "repo": repo,
        "gate": gate,
        "base": base,
        "collected_at": datetime.now(timezone.utc).isoformat(),
        "prs": [],
        "errors": [],
        "required_contexts": [],
    }

    # The workflow a FRESH pull_request run would use is the one on the base
    # branch, because the merge commit is recomputed against it.
    try:
        blob = _gh_json(gh_bin, f"repos/{repo}/contents/{workflow_path}?ref={base}")
        snapshot["workflow_text"] = base64.b64decode(blob["content"]).decode("utf-8")
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError, ValueError) as exc:
        snapshot["workflow_text"] = None
        snapshot["workflow_error"] = f"could not read {workflow_path}: {str(exc)[:200]}"
        snapshot["errors"].append({"stage": "workflow", "error": str(exc)[:200]})

    snapshot["required_contexts"] = _required_contexts(gh_bin, repo, base)

    try:
        batches = _merge_group_failures(gh_bin, repo)
    except (subprocess.CalledProcessError, json.JSONDecodeError, ValueError, AttributeError) as exc:
        batches = {}
        snapshot["errors"].append({"stage": "merge_group", "error": str(exc)[:200]})

    # Queue membership only exists in GraphQL. REST cannot answer it, and
    # `auto_merge` is not a proxy: enqueueing CONSUMES the auto-merge request.
    nodes: list[dict[str, Any]] = []
    cursor: str | None = None
    owner, _, name = repo.partition("/")
    try:
        while True:
            page = _graphql(
                gh_bin, _PR_QUERY, owner=owner, name=name, base=base, cursor=cursor
            )
            block = page["data"]["repository"]["pullRequests"]
            nodes.extend(block.get("nodes") or [])
            if not (block.get("pageInfo") or {}).get("hasNextPage"):
                break
            cursor = block["pageInfo"]["endCursor"]
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError, TypeError) as exc:
        snapshot["errors"].append({"stage": "pulls", "error": str(exc)[:200]})
        return snapshot

    for node in nodes:
        number = node.get("number")
        record: dict[str, Any] = {
            "number": number,
            "title": node.get("title"),
            "errors": [],
            "mergeable_state": str(node.get("mergeStateStatus") or "").lower(),
            "auto_merge": node.get("autoMergeRequest") is not None,
            "draft": bool(node.get("isDraft")),
            "head_sha": node.get("headRefOid"),
            "updated_at": node.get("updatedAt"),
            "in_merge_queue": bool(node.get("isInMergeQueue")),
            "merge_queue_state": ((node.get("mergeQueueEntry") or {}) or {}).get("state"),
        }

        sha = record.get("head_sha")
        try:
            runs, total = _paginate(
                gh_bin,
                f"repos/{repo}/commits/{sha}/check-runs?filter=latest",
                "check_runs",
            )
            if total is not None and len(runs) != total:
                raise ValueError(
                    f"check-run listing is truncated: {len(runs)} of {total}"
                )
            record["checks"] = [
                {
                    "name": c.get("name"),
                    "status": c.get("status"),
                    "conclusion": c.get("conclusion"),
                    "started_at": c.get("started_at"),
                    "completed_at": c.get("completed_at"),
                    "run_id": _run_id_from(c),
                    "job_id": _job_id_from(c),
                }
                for c in runs
            ]
            stamps = [
                str(c.get("completed_at") or c.get("started_at") or "")
                for c in record["checks"]
            ]
            record["last_ci_at"] = max([s for s in stamps if s], default=None)
        except (subprocess.CalledProcessError, json.JSONDecodeError, AttributeError, ValueError) as exc:
            record["errors"].append(f"check runs: {str(exc)[:160]}")
            snapshot["prs"].append(record)
            continue

        if not record["in_merge_queue"]:
            batch = batches.get(int(number or 0))
            superseded = batch is not None and str(record.get("last_ci_at") or "") > str(
                batch.get("at") or ""
            )
            if (
                batch
                and not superseded
                and batch.get("conclusion") not in ("success", "skipped", None)
            ):
                record["merge_group_failure"] = batch
            try:
                record.update(_timeline_signals(gh_bin, repo, int(number)))
            except (subprocess.CalledProcessError, json.JSONDecodeError, ValueError, TypeError) as exc:
                record["errors"].append(f"timeline: {str(exc)[:160]}")
                snapshot["prs"].append(record)
                continue

        gate_run = _gate_check(record, gate)
        if gate_run and str(gate_run.get("conclusion") or "").lower() not in GREEN:
            run_id = gate_run.get("run_id")
            job_id = gate_run.get("job_id")
            if run_id is None:
                record["errors"].append("gate check has no workflow run id")
            elif job_id is None:
                # Without the job id the steps cannot be attributed, and
                # sweeping every failed job in the run blames the gate for
                # another leg's failure. Refuse rather than approximate.
                record["errors"].append("gate check has no job id in details_url")
            else:
                try:
                    jobs, _ = _paginate(
                        gh_bin, f"repos/{repo}/actions/runs/{run_id}/jobs", "jobs"
                    )
                    job = next((j for j in jobs if j.get("id") == job_id), None)
                    if job is None:
                        record["errors"].append(
                            f"gate job {job_id} is absent from run {run_id}"
                        )
                    else:
                        record["gate_job_conclusion"] = job.get("conclusion")
                        record["gate_runner_os"] = _runner_os(job)
                        record["gate_failed_steps"] = [
                            str(step.get("name"))
                            for step in job.get("steps") or []
                            if str(step.get("conclusion") or "").lower() == "failure"
                        ]
                except (subprocess.CalledProcessError, json.JSONDecodeError, ValueError) as exc:
                    record["errors"].append(f"gate jobs: {str(exc)[:160]}")
        snapshot["prs"].append(record)

    return snapshot


def _run_id_from(check: dict[str, Any]) -> int | None:
    """Recover the Actions run id a check run belongs to.

    The check-run id is not the job id and must never be used as one; the run
    id is only reliably present in details_url.
    """
    url = str(check.get("details_url") or "")
    match = re.search(r"/actions/runs/(\d+)", url)
    return int(match.group(1)) if match else None


def _job_id_from(check: dict[str, Any]) -> int | None:
    """Recover the Actions JOB id a check run belongs to.

    Steps must be harvested from this job alone. A run holds every platform
    leg, so sweeping all its failed jobs attributes another leg's failure to
    the gate — and that misattribution defeats exactly the case this tool was
    written for, where the gate's own failing step is the whole question.
    """
    url = str(check.get("details_url") or "")
    match = re.search(r"/actions/runs/\d+/job/(\d+)", url)
    return int(match.group(1)) if match else None


# --------------------------------------------------------------------------
# Fix actions
# --------------------------------------------------------------------------


def select_fixable(findings: list[Finding]) -> list[Finding]:
    """The only findings --fix may act on.

    This is the whole safety property in one place: a PR reaches a mutation
    only by being classified AUTO-FIXABLE. UNKNOWN and NEEDS-HUMAN are excluded
    here, and `apply_fix` refuses them again independently.
    """
    return [f for f in findings if f.bucket == AUTO_FIXABLE]


def cap_fixes(findings: list[Finding], max_fixes: int) -> tuple[list[Finding], int]:
    """The fixable findings this sweep may act on, and how many it withheld.

    A classifier bug does not mislabel one PR, it mislabels a shape of PR — so
    the blast radius of a wrong verdict is the whole backlog. Capping turns
    that into a handful of actions a human can still read and undo.
    """
    fixable = select_fixable(findings)
    if max_fixes < 0 or len(fixable) <= max_fixes:
        return fixable, 0
    return fixable[:max_fixes], len(fixable) - max_fixes


def apply_fix(finding: Finding, repo: str, gh_bin: str, dry_run: bool) -> str:
    """Perform one auto-fixable action. Only ever called for AUTO-FIXABLE."""
    if finding.bucket != AUTO_FIXABLE:
        raise ValueError(f"refusing to fix a {finding.bucket} pull request")
    number = finding.number
    if finding.action == "enable-auto-merge":
        if dry_run:
            return f"would arm auto-merge (MERGE) on #{number}"
        node = _gh_json(gh_bin, f"repos/{repo}/pulls/{number}")["node_id"]
        query = (
            "mutation($id:ID!){enablePullRequestAutoMerge("
            "input:{pullRequestId:$id,mergeMethod:MERGE}){clientMutationId}}"
        )
        _gh(gh_bin, "api", "graphql", "-f", f"query={query}", "-f", f"id={node}")
        return f"armed auto-merge on #{number}"
    if finding.action == "update-branch":
        if dry_run:
            return f"would update #{number} from base to pick up the current workflow"
        _gh(gh_bin, "api", "-X", "PUT", f"repos/{repo}/pulls/{number}/update-branch")
        return f"updated #{number} from base"
    if finding.action == "rerun-failed-jobs":
        run_id = finding.detail.get("run_id")
        if not run_id:
            raise ValueError(f"#{number} has no run id to replay")
        if dry_run:
            return f"would replay lost run {run_id} for #{number}"
        _gh(gh_bin, "api", "-X", "POST", f"repos/{repo}/actions/runs/{run_id}/rerun-failed-jobs")
        return f"replayed run {run_id} for #{number}"
    raise ValueError(f"unknown action {finding.action!r}")


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def render_report(findings: list[Finding], snapshot: dict[str, Any]) -> str:
    """Render the human report. Returns a string; never prints."""
    lines: list[str] = []
    degraded = bool(snapshot.get("errors"))
    total = len(findings)
    counts = {b: sum(1 for f in findings if f.bucket == b) for b in BUCKET_ORDER}
    lines.append(f"PR flow audit — {total} open pull requests")
    lines.append(
        "  " + "  ".join(f"{bucket}={counts[bucket]}" for bucket in BUCKET_ORDER)
    )
    lines.append("")

    for bucket in BUCKET_ORDER:
        rows = [f for f in findings if f.bucket == bucket]
        if not rows:
            continue
        lines.append(f"{bucket} ({len(rows)})")
        for finding in rows:
            marker = " *CHEAPEST*" if finding.cheap_win else ""
            lines.append(f"  #{finding.number}{marker} {finding.title[:64]}")
            lines.append(f"      {finding.reason}")
            if finding.action:
                lines.append(f"      action: {finding.action}")
        lines.append("")

    stalled = counts[NEEDS_HUMAN] + counts[AUTO_FIXABLE] + counts[UNKNOWN]
    if degraded:
        lines.append(
            "This sweep's evidence was incomplete, so it is not a clean bill of health."
        )
    elif stalled == 0:
        lines.append("Every open pull request has work in flight.")
    else:
        lines.append(f"{stalled} pull requests are not moving.")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default="Generous-Corp/pulp", help="owner/name")
    ap.add_argument(
        "--gh",
        default=os.environ.get("PULP_GH_BIN") or "ghapp",
        help="gh CLI to use (default: ghapp — App token, higher rate limit).",
    )
    ap.add_argument("--gate", default=DEFAULT_GATE, help="required check to audit")
    ap.add_argument("--workflow", default=DEFAULT_WORKFLOW, help="workflow owning the gate")
    ap.add_argument("--base", default="main", help="base branch whose workflow a fresh run uses")
    ap.add_argument("--snapshot", default="", help="read a recorded snapshot instead of the API")
    ap.add_argument("--snapshot-out", default="", help="write the collected snapshot here")
    ap.add_argument("--json", action="store_true", help="emit findings as JSON on stdout")
    ap.add_argument("--fix", action="store_true", help="act on AUTO-FIXABLE findings")
    ap.add_argument(
        "--apply",
        action="store_true",
        help="with --fix, actually mutate. Without it --fix only reports what it would do.",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="explicit no-mutation mode (the default; kept so intent can be stated)",
    )
    ap.add_argument(
        "--max-fixes",
        type=int,
        default=DEFAULT_MAX_FIXES,
        help="most mutations one sweep may perform (-1 for no cap)",
    )
    ap.add_argument(
        "--in-flight-hours",
        type=float,
        default=DEFAULT_IN_FLIGHT_HOURS,
        help="hours a check may pend before it stops counting as progress",
    )
    ap.add_argument(
        "--unstable-hours",
        type=float,
        default=DEFAULT_UNSTABLE_HOURS,
        help="hours of unattended instability before a PR needs a human",
    )
    args = ap.parse_args(argv)

    if args.unstable_hours < 0:
        print("pr_flow_audit: --unstable-hours must be >= 0", file=sys.stderr)
        return 2
    if args.fix and args.snapshot:
        print("pr_flow_audit: --fix needs live state, not a snapshot", file=sys.stderr)
        return 2

    if args.snapshot:
        snapshot = json.loads(open(args.snapshot, encoding="utf-8").read())
    else:
        snapshot = collect(args.repo, args.gh, args.gate, args.workflow, args.base)

    if args.snapshot_out:
        with open(args.snapshot_out, "w", encoding="utf-8") as handle:
            json.dump(snapshot, handle, indent=2, sort_keys=True)

    now = datetime.now(timezone.utc)
    findings = analyze(
        snapshot,
        now,
        gate=args.gate,
        unstable_hours=args.unstable_hours,
        in_flight_hours=args.in_flight_hours,
    )

    applied: list[str] = []
    failures = 0
    if args.fix:
        # Mutating is opt-in. A tool whose default run can merge pull requests
        # is one nobody can safely put on a timer.
        dry_run = not args.apply or args.dry_run
        selected, withheld = cap_fixes(findings, args.max_fixes)
        for finding in selected:
            try:
                applied.append(apply_fix(finding, args.repo, args.gh, dry_run))
            except (subprocess.CalledProcessError, ValueError, KeyError, json.JSONDecodeError) as exc:
                failures += 1
                applied.append(f"#{finding.number}: FAILED — {str(exc)[:160]}")
        if withheld:
            applied.append(
                f"withheld {withheld} further fix(es): --max-fixes={args.max_fixes}"
            )

    if args.json:
        payload = {
            "repo": snapshot.get("repo"),
            "collected_at": snapshot.get("collected_at"),
            "degraded": bool(snapshot.get("errors")),
            "errors": snapshot.get("errors") or [],
            "findings": [f.to_json() for f in findings],
            "applied": applied,
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print(render_report(findings, snapshot))
        for line in applied:
            print(f"  fix: {line}")

    stalled = sum(
        1 for f in findings if f.bucket in (NEEDS_HUMAN, AUTO_FIXABLE, UNKNOWN)
    )
    # The machine-readable handoff line. Under --json it goes to stderr so
    # stdout stays a single parseable document.
    print(f"stalled_count={stalled}", file=sys.stderr if args.json else sys.stdout)
    # Findings alone never redden the run: an auditor that fails on what it was
    # built to find is one nobody keeps green. A fix that FAILED is different —
    # it means a mutation this tool chose to make did not happen, and a caller
    # that cannot see that has no way to know the sweep left work undone.
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
