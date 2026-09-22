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

Fail closed
-----------
A false "nothing to do" is the failure mode being eliminated, so it must be
impossible to emit. Any PR whose state cannot be established — API error, absent
checks, unparseable or ambiguous workflow condition — is reported UNKNOWN with
the reason, and `--fix` refuses to touch it.
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

import yaml

# The workflow that owns the required gate, and the event a pull request runs
# under. Both are inputs to reachability, not assertions about any step.
DEFAULT_WORKFLOW = ".github/workflows/build.yml"
PULL_REQUEST_EVENT = "pull_request"
DEFAULT_GATE = "macos"
DEFAULT_UNSTABLE_HOURS = 6.0

# Conclusions that leave a check with nothing outstanding.
GREEN = {"success", "skipped", "neutral"}
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
            return not _truthy(self.parse_unary())
        return self.parse_comparison()

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

    An absent condition means the step always runs. Status functions are pinned
    to the branch that makes the step run at all — the question being asked is
    "could this step execute under this event", not "did it".
    """
    if expr is None:
        return True
    if isinstance(expr, bool):
        return expr
    text = _strip_wrappers(expr)
    if not text:
        return True
    context = {
        "values": {
            "github.event_name": event_name,
            "runner.os": runner_os,
        },
        "functions": {
            "success": True,
            "failure": True,
            "always": True,
            "cancelled": True,
        },
    }
    return _truthy(_Parser(_tokenize(text), context).parse())


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


def parse_workflow_steps(text: str) -> dict[str, StepCondition]:
    """Map step name -> condition for every named step in a workflow."""
    try:
        document = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise WorkflowError(f"workflow is not valid YAML: {exc}") from exc
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
    name: str, steps: dict[str, StepCondition]
) -> StepVerdict:
    """Decide whether a failing step would block a fresh pull_request run."""
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
        condition.expr, event_name=PULL_REQUEST_EVENT, runner_os="macOS"
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


def _any_work_in_flight(pr: dict[str, Any]) -> bool:
    return any(
        str(c.get("status") or "").lower() in PENDING_STATUS
        for c in pr.get("checks") or []
    )


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

    state = str(pr.get("mergeable_state") or "").lower()
    if not state or state == "unknown":
        base.reason = "GitHub has not computed a mergeable state yet"
        return base

    if pr.get("draft"):
        base.bucket = NEEDS_HUMAN
        base.reason = "draft — waiting on its author to mark it ready"
        base.action = "author marks ready for review"
        return base

    if pr.get("in_merge_queue"):
        base.bucket = MOVING
        base.reason = "in the merge queue"
        return base

    checks = pr.get("checks")
    if checks is None:
        base.reason = "check runs could not be read"
        return base

    if _any_work_in_flight(pr):
        base.bucket = MOVING
        base.reason = "a check is queued or running"
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
        if steps is None:
            base.reason = (
                f"{gate} failed, and reachability is undecidable: {workflow_error}"
            )
            return base
        failed_steps = pr.get("gate_failed_steps")
        if failed_steps is None:
            base.reason = f"{gate} failed but its steps could not be read"
            return base
        if not failed_steps:
            # The job reported failure while no step did: the run was lost
            # rather than the change rejected. Replaying it is exactly right.
            base.bucket = AUTO_FIXABLE
            base.reason = f"{gate} failed with no failing step — the run was lost"
            base.action = "rerun-failed-jobs"
            base.detail = {"run_id": gate_run.get("run_id")}
            return base
        try:
            verdicts = [classify_failing_step(s, steps) for s in failed_steps]
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
        age = _hours_since(pr.get("updated_at"), now)
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


def collect(
    repo: str, gh_bin: str, gate: str, workflow_path: str, base: str = "main"
) -> dict[str, Any]:
    """Build a snapshot. Every read that fails is recorded, never defaulted."""
    snapshot: dict[str, Any] = {
        "repo": repo,
        "gate": gate,
        "base": base,
        "collected_at": datetime.now(timezone.utc).isoformat(),
        "prs": [],
        "errors": [],
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

    try:
        listing = _gh_json(gh_bin, f"repos/{repo}/pulls?state=open&per_page=100")
    except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        snapshot["errors"].append({"stage": "pulls", "error": str(exc)[:200]})
        return snapshot

    for item in listing:
        number = item.get("number")
        record: dict[str, Any] = {
            "number": number,
            "title": item.get("title"),
            "errors": [],
        }
        try:
            detail = _gh_json(gh_bin, f"repos/{repo}/pulls/{number}")
            record.update(
                {
                    "mergeable_state": detail.get("mergeable_state"),
                    "auto_merge": detail.get("auto_merge") is not None,
                    "draft": bool(detail.get("draft")),
                    "head_sha": (detail.get("head") or {}).get("sha"),
                    "updated_at": detail.get("updated_at"),
                }
            )
        except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            record["errors"].append(f"pull detail: {str(exc)[:160]}")
            snapshot["prs"].append(record)
            continue

        sha = record.get("head_sha")
        try:
            runs = _gh_json(
                gh_bin,
                f"repos/{repo}/commits/{sha}/check-runs?per_page=100&filter=latest",
            )
            record["checks"] = [
                {
                    "name": c.get("name"),
                    "status": c.get("status"),
                    "conclusion": c.get("conclusion"),
                    "started_at": c.get("started_at"),
                    "run_id": _run_id_from(c),
                }
                for c in runs.get("check_runs") or []
            ]
        except (subprocess.CalledProcessError, json.JSONDecodeError, AttributeError) as exc:
            record["errors"].append(f"check runs: {str(exc)[:160]}")
            snapshot["prs"].append(record)
            continue

        gate_run = _gate_check(record, gate)
        if gate_run and str(gate_run.get("conclusion") or "").lower() not in GREEN:
            run_id = gate_run.get("run_id")
            if run_id is None:
                record["errors"].append("gate check has no workflow run id")
            else:
                try:
                    jobs = _gh_json(
                        gh_bin, f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"
                    )
                    failed: list[str] = []
                    for job in jobs.get("jobs") or []:
                        if str(job.get("conclusion") or "").lower() != "failure":
                            continue
                        for step in job.get("steps") or []:
                            if str(step.get("conclusion") or "").lower() == "failure":
                                failed.append(str(step.get("name")))
                    record["gate_failed_steps"] = failed
                except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
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
    ap.add_argument("--fix", action="store_true", help="perform AUTO-FIXABLE actions")
    ap.add_argument("--dry-run", action="store_true", help="with --fix, print without mutating")
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
        snapshot, now, gate=args.gate, unstable_hours=args.unstable_hours
    )

    applied: list[str] = []
    if args.fix:
        for finding in select_fixable(findings):
            try:
                applied.append(apply_fix(finding, args.repo, args.gh, args.dry_run))
            except (subprocess.CalledProcessError, ValueError, KeyError, json.JSONDecodeError) as exc:
                applied.append(f"#{finding.number}: FAILED — {str(exc)[:160]}")

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
    # Exit 0 regardless of findings: the caller decides what to do with them.
    # An auditor that reddens its own run is an auditor nobody keeps green.
    return 0


if __name__ == "__main__":
    sys.exit(main())
