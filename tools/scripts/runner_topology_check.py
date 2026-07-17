#!/usr/bin/env python3
"""Runner-routing black-hole gate.

Reconciles the declared routing contract (runner_topology.json) against the
LIVE repo variables and the LIVE registered runners, and fails when a lane
targets a label set nothing can serve.

WHY THIS EXISTS
    GitHub does not validate `runs-on`. A job requesting a label no runner
    carries is not an error — it is QUEUED, forever, indistinguishable from
    "the pool is busy". So a routing variable can be edited to point at a
    label that does not exist and the only symptom is jobs piling up while
    the pool looks saturated. A relief valve routed into a black hole is
    worse than no relief valve: it reports healthy and relieves nothing.

WHAT IT CHECKS
    drift        — live variable value != the lane's contracted `expect`.
                   Makes the variable a reviewed artifact instead of a
                   blind edit.
    undeclared   — a live `*_RUNS_ON_JSON` variable with no lane. A new lane
                   added without a contract row.
    black-hole   — the lane's labels are satisfiable by no runner.
    degraded     — the only runners that match are offline (may just be asleep).
    hosted       — a GitHub-hosted scalar outside the allowlist (typo catch).
    must-unset   — a variable contracted to stay unset is set (cost guard).

LABEL MATCHING
    GitHub dispatches a job to a runner only when the runner carries EVERY
    label in the `runs-on` array. Matching here is subset containment, not
    "any label overlaps".

THREE RUNNER STATES, NOT TWO
    online / offline is not the whole story. Tart runners register JIT and
    ephemeral (tools/ci/tart-runner.sh): they exist only while a job runs.
    For those lanes "no runner carries this label" proves nothing on its own —
    the pool may simply be idle. So an ephemeral lane with no live match is
    adjudicated on SERVICE HISTORY instead: a label set that has served no job
    in the lookback window has no provisioner behind it, and that is a black
    hole. A persistent lane is adjudicated on the registry directly.

    python3 tools/scripts/runner_topology_check.py --mode=report   # exit 1 on error
    python3 tools/scripts/runner_topology_check.py --mode=hint     # advisory
    python3 tools/scripts/runner_topology_check.py --runners-json fixtures/r.json \
        --variables-json fixtures/v.json --jobs-json fixtures/j.json   # offline
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
DEFAULT_CONTRACT = HERE / "runner_topology.json"
DEFAULT_REPO = "danielraffel/pulp"

SELF_HOSTED = "self-hosted"

ERROR = "error"
WARN = "warn"
OK = "ok"


# ── Types ───────────────────────────────────────────────────────────────


@dataclass
class Lane:
    variable: str
    purpose: str
    expect: Any
    provisioning: str
    severity: str
    hosts: list[str] = field(default_factory=list)
    # What the consuming workflow's `|| <default>` supplies when the variable
    # is unset. None means the workflow has no fallback, so unset = no route.
    unset_fallback: Any = None

    @property
    def is_self_hosted(self) -> bool:
        return isinstance(self.expect, list) and SELF_HOSTED in self.expect


@dataclass
class Runner:
    name: str
    status: str
    labels: set[str]

    @property
    def online(self) -> bool:
        return self.status == "online"


@dataclass
class Contract:
    lanes: list[Lane]
    github_hosted_labels: set[str]
    sentinels: set[str]
    must_remain_unset: list[str]
    must_remain_unset_why: str
    lookback_hours: int
    runs_per_workflow: int


@dataclass
class Finding:
    level: str
    kind: str
    variable: str
    detail: str


# ── Contract loading ────────────────────────────────────────────────────


def load_contract(path: Path) -> Contract:
    data = json.loads(path.read_text())
    lanes = [
        Lane(
            variable=raw["variable"],
            purpose=raw.get("purpose", ""),
            expect=raw["expect"],
            provisioning=raw["provisioning"],
            severity=raw.get("severity", "advisory"),
            hosts=raw.get("hosts", []) or [],
            unset_fallback=raw.get("unset_fallback"),
        )
        for raw in data.get("lanes", [])
    ]
    unset = data.get("must_remain_unset", {}) or {}
    evidence = data.get("service_evidence", {}) or {}
    return Contract(
        lanes=lanes,
        github_hosted_labels=set(data.get("github_hosted_labels", [])),
        sentinels=set(data.get("sentinels", [])),
        must_remain_unset=list(unset.get("variables", [])),
        must_remain_unset_why=unset.get("why", ""),
        lookback_hours=int(evidence.get("lookback_hours", 168)),
        runs_per_workflow=int(evidence.get("runs_per_workflow", 20)),
    )


# ── Live state ──────────────────────────────────────────────────────────


def resolve_cli() -> str:
    """Pick the GitHub CLI to call.

    Locally, `ghapp` authenticates as the Shipyard GitHub App and draws on its
    own 12,500/hr bucket; plain `gh` burns the personal 5,000/hr token shared
    with the human and trips secondary rate limits. So `ghapp` wins whenever it
    exists. On a GitHub-hosted runner it does not exist, and the workflow
    supplies a token via GH_TOKEN instead.
    """
    override = os.environ.get("PULP_GH_CLI")
    if override:
        return override
    return "ghapp" if shutil.which("ghapp") else "gh"


def _api(args: list[str]) -> Any:
    out = subprocess.run(
        [resolve_cli(), "api", *args],
        check=True, capture_output=True, text=True,
    )
    return json.loads(out.stdout)


def fetch_runners(repo: str) -> list[Runner]:
    data = _api([f"repos/{repo}/actions/runners", "--paginate"])
    return parse_runners(data)


def parse_runners(data: Any) -> list[Runner]:
    runners = data.get("runners", data) if isinstance(data, dict) else data
    return [
        Runner(
            name=r["name"],
            status=r.get("status", "offline"),
            labels={lbl["name"] if isinstance(lbl, dict) else lbl
                    for lbl in r.get("labels", [])},
        )
        for r in runners
    ]


def fetch_variables(repo: str) -> dict[str, str]:
    data = _api([f"repos/{repo}/actions/variables", "--paginate"])
    return parse_variables(data)


def parse_variables(data: Any) -> dict[str, str]:
    items = data.get("variables", data) if isinstance(data, dict) else data
    return {v["name"]: v["value"] for v in items}


def find_consuming_workflows(variable: str, workflows_dir: Path) -> list[str]:
    """Which workflow files route jobs using this variable?

    Scanning a lane's OWN workflow is what makes service history meaningful.
    A repo-wide "last N runs" scan is not a time window at all on a busy repo:
    measured here, 100 runs covered well under an hour, so a lane used by a
    weekly release would look unserved — and be condemned as a black hole —
    every single sweep. Scoping to the consuming workflow means 20 runs of
    `release-cli.yml` reach back months for a handful of API calls.
    """
    needle = f"vars.{variable}"
    found: list[str] = []
    if not workflows_dir.is_dir():
        return found
    for path in sorted(workflows_dir.glob("*.yml")) + sorted(workflows_dir.glob("*.yaml")):
        try:
            if needle in path.read_text():
                found.append(path.name)
        except OSError:
            continue
    return found


def fetch_served_label_sets(
    repo: str,
    lookback_hours: int,
    workflows: list[str],
    runs_per_workflow: int,
) -> list[set[str]]:
    """Label sets a consuming workflow actually dispatched jobs to.

    The jobs API reports each job's REQUESTED labels, which is exactly the
    routing question: was anything willing to serve this label set? Bounded on
    both axes — `lookback_hours` is the window the verdict claims, and
    `runs_per_workflow` caps API cost so an hourly cron cannot walk unbounded
    history.
    """
    cutoff = datetime.now(timezone.utc) - timedelta(hours=lookback_hours)
    served: list[set[str]] = []
    for wf in workflows:
        try:
            runs = _api([
                f"repos/{repo}/actions/workflows/{wf}/runs"
                f"?per_page={min(runs_per_workflow, 100)}",
            ])
        except subprocess.CalledProcessError:
            continue
        for run in runs.get("workflow_runs", [])[:runs_per_workflow]:
            created = run.get("created_at")
            if created and _parse_ts(created) < cutoff:
                break  # newest-first: the rest of this workflow is older
            try:
                jobs = _api([f"repos/{repo}/actions/runs/{run['id']}/jobs"])
            except subprocess.CalledProcessError:
                continue
            for job in jobs.get("jobs", []):
                # A job that never started proves nothing: a QUEUED job is the
                # black-hole symptom itself, not evidence of service.
                if job.get("status") == "queued":
                    continue
                served.append(set(job.get("labels", [])))
    return served


def _parse_ts(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def parse_served_label_sets(data: Any) -> list[set[str]]:
    return [set(entry) for entry in data]


# ── Matching ────────────────────────────────────────────────────────────


SELF_HOSTED_KIND = "self-hosted"
HOSTED_KIND = "github-hosted"
SENTINEL_KIND = "sentinel"
UNKNOWN_KIND = "unknown"


def classify_target(target: Any, contract: Contract) -> str:
    """What kind of `runs-on` value is this?

    GitHub's own discriminator is the literal `self-hosted` label, so an array
    carrying it is a self-hosted label set. Everything else must be named in an
    allowlist rather than assumed: `runs-on: [macos-15]` is a perfectly legal
    hosted array, and `runs-on: [macos-15x]` is a typo that queues forever, and
    nothing structural distinguishes them.
    """
    if isinstance(target, str):
        if target in contract.sentinels:
            return SENTINEL_KIND
        if target in contract.github_hosted_labels:
            return HOSTED_KIND
        return UNKNOWN_KIND
    if isinstance(target, list):
        if SELF_HOSTED in target:
            return SELF_HOSTED_KIND
        if target and all(item in contract.github_hosted_labels for item in target):
            return HOSTED_KIND
        return UNKNOWN_KIND
    return UNKNOWN_KIND


def matching_runners(labels: list[str], runners: list[Runner]) -> list[Runner]:
    """Runners that satisfy EVERY requested label (GitHub's own rule)."""
    want = set(labels)
    return [r for r in runners if want <= r.labels]


def has_service_evidence(labels: list[str], served: list[set[str]]) -> bool:
    """Did any dispatched job request exactly this lane's label set?

    Exact equality, not containment: a job served with a superset was routed
    by a different lane and says nothing about whether THIS lane's labels have
    a provisioner.
    """
    want = set(labels)
    return any(want == s for s in served)


# ── Checks ──────────────────────────────────────────────────────────────


def _level_for(lane: Lane) -> str:
    return ERROR if lane.severity == "required" else WARN


def static_evidence(served: list[set[str]]):
    """An evidence provider backed by a fixed list (fixtures, tests)."""
    return lambda _lane: served


def check(
    contract: Contract,
    runners: list[Runner],
    variables: dict[str, str],
    evidence: Any,
) -> list[Finding]:
    """`evidence` is a callable lane -> list of served label sets, invoked ONLY
    when an ephemeral lane has no live runner. A list is accepted for
    convenience and wrapped. Laziness matters: the scan costs API calls, and a
    healthy fleet must not pay for them every sweep."""
    if not callable(evidence):
        evidence = static_evidence(evidence)
    findings: list[Finding] = []
    declared = {lane.variable for lane in contract.lanes}

    # Cost guard: a paid-overflow variable that is set at all.
    for name in contract.must_remain_unset:
        if name in variables:
            findings.append(Finding(
                ERROR, "must-unset", name,
                f"contracted to stay UNSET but is set to {variables[name]!r}. "
                f"{contract.must_remain_unset_why}",
            ))

    # A live routing variable nobody declared — a lane added blind.
    for name in sorted(variables):
        if not name.endswith("_RUNS_ON_JSON"):
            continue
        if name in declared or name in contract.must_remain_unset:
            continue
        findings.append(Finding(
            ERROR, "undeclared", name,
            f"set to {variables[name]!r} but has no lane in runner_topology.json. "
            "Every routing variable must be a reviewed artifact: add a lane "
            "declaring the labels it is intended to route to.",
        ))

    for lane in contract.lanes:
        findings.extend(_check_lane(lane, contract, runners, variables, evidence))

    return findings


def _check_lane(
    lane: Lane,
    contract: Contract,
    runners: list[Runner],
    variables: dict[str, str],
    evidence: Any,
) -> list[Finding]:
    findings: list[Finding] = []

    if lane.variable not in variables:
        # Unset is not automatically broken. A workflow usually supplies its own
        # `|| <default>` when the variable is empty (GitHub treats unset and
        # empty identically), so the lane still routes — to the fallback. The
        # fallback is what actually runs jobs, so it is what gets adjudicated.
        if lane.unset_fallback is None:
            findings.append(Finding(
                _level_for(lane), "unset", lane.variable,
                f"declared as a {lane.severity} lane with no documented "
                "workflow fallback, but the variable is not set. "
                f"Purpose: {lane.purpose}",
            ))
            return findings
        return _check_target(lane, contract, runners, evidence, lane.unset_fallback,
                             origin="workflow fallback (variable unset)")

    # ── drift: the variable must match what the contract says it routes to.
    actual_raw = variables[lane.variable]
    try:
        actual = json.loads(actual_raw)
    except json.JSONDecodeError:
        findings.append(Finding(
            ERROR, "malformed", lane.variable,
            f"value {actual_raw!r} is not valid JSON; `fromJSON()` in the "
            "workflow will fail at dispatch.",
        ))
        return findings

    if actual != lane.expect:
        findings.append(Finding(
            ERROR, "drift", lane.variable,
            f"live value {json.dumps(actual)} != contracted {json.dumps(lane.expect)}. "
            "The variable was edited without updating the contract (or vice "
            "versa). Reconcile both together.",
        ))
        # Adjudicate satisfiability against what is LIVE — that is what is
        # actually routing jobs right now, whatever the contract wishes.
        target = actual
    else:
        target = lane.expect

    findings.extend(_check_target(lane, contract, runners, evidence, target,
                                  origin="variable"))
    return findings


def _check_target(
    lane: Lane,
    contract: Contract,
    runners: list[Runner],
    evidence: Any,
    target: Any,
    origin: str,
) -> list[Finding]:
    """Can anything actually serve this `runs-on` value?"""
    findings: list[Finding] = []
    kind = classify_target(target, contract)
    shown = json.dumps(target)

    if kind == SENTINEL_KIND:
        return findings  # an explicit off-switch, not a routing target

    if kind == HOSTED_KIND:
        return findings  # GitHub provides the capacity

    if kind == UNKNOWN_KIND:
        findings.append(Finding(
            ERROR, "hosted-unknown", lane.variable,
            f"{shown} ({origin}) carries no {SELF_HOSTED!r} label, so GitHub "
            "reads it as hosted image name(s), but it is not in "
            "`github_hosted_labels` or `sentinels`. Either it is a typo (jobs "
            "queue forever) or the allowlist needs the new image.",
        ))
        return findings

    # ── self-hosted lanes: can anything serve this label set?
    matches = matching_runners(target, runners)
    online = [r for r in matches if r.online]
    labels_str = json.dumps(target)

    if online:
        return findings

    if matches:
        # Registered but all offline: may just be asleep (m1 is intermittent).
        # A different failure from a label nothing owns — do not conflate.
        findings.append(Finding(
            WARN, "degraded", lane.variable,
            f"{labels_str} is satisfied only by OFFLINE runners "
            f"({', '.join(sorted(r.name for r in matches))}). The host may be "
            "asleep; jobs queue until it returns.",
        ))
        return findings

    if lane.provisioning == "ephemeral":
        # No live runner proves nothing for a JIT lane — check service history.
        if has_service_evidence(target, evidence(lane)):
            findings.append(Finding(
                OK, "ephemeral-idle", lane.variable,
                f"{labels_str} has no live runner, but jobs were served on it "
                "within the lookback window: the provisioner is alive and idle.",
            ))
            return findings
        findings.append(Finding(
            _level_for(lane), "black-hole", lane.variable,
            f"{labels_str} is carried by NO registered runner and has served NO "
            f"job in the last {contract.lookback_hours}h. Nothing provisions it. "
            "Jobs routed here queue forever with no error. "
            f"Purpose: {lane.purpose}",
        ))
        return findings

    findings.append(Finding(
        _level_for(lane), "black-hole", lane.variable,
        f"{labels_str} is carried by NO registered runner (online or offline). "
        "Jobs routed here queue forever with no error. "
        f"Purpose: {lane.purpose}",
    ))
    return findings


# ── Reporting ───────────────────────────────────────────────────────────


def render(findings: list[Finding]) -> str:
    lines: list[str] = []
    errors = [f for f in findings if f.level == ERROR]
    warns = [f for f in findings if f.level == WARN]

    for f in errors:
        lines.append(f"  ERROR [{f.kind}] {f.variable}")
        lines.append(f"         {f.detail}")
    for f in warns:
        lines.append(f"  WARN  [{f.kind}] {f.variable}")
        lines.append(f"         {f.detail}")

    if errors:
        lines.append("")
        lines.append(f"runner-topology: {len(errors)} error(s), {len(warns)} warning(s).")
        lines.append("A lane pointing at labels nothing serves is SILENT: GitHub")
        lines.append("queues the job instead of failing it, so the symptom is")
        lines.append("'jobs pile up while the pool looks busy', never an error.")
        lines.append("Fix by reconciling the repo variable and this contract, or by")
        lines.append("bringing the provisioner for those labels back online.")
    elif warns:
        lines.append("")
        lines.append(f"runner-topology: OK ({len(warns)} warning(s)).")
    else:
        lines.append("runner-topology: OK — every lane resolves to a live runner.")
    return "\n".join(lines)


# ── Entry point ─────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["hint", "report"], default="report",
                    help="report: exit 1 on error. hint: advisory, always exit 0.")
    ap.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--runners-json", type=Path,
                    help="Offline fixture instead of a live API call.")
    ap.add_argument("--variables-json", type=Path)
    ap.add_argument("--jobs-json", type=Path,
                    help="Fixture of served label sets (list of label lists).")
    ap.add_argument("--workflows-dir", type=Path,
                    help="Workflow directory used to find a lane's consumers.")
    ap.add_argument("--json", action="store_true", help="Emit findings as JSON.")
    args = ap.parse_args(argv)

    contract = load_contract(args.contract)

    offline_inputs = bool(args.runners_json and args.variables_json)
    if offline_inputs:
        runners = parse_runners(json.loads(args.runners_json.read_text()))
        variables = parse_variables(json.loads(args.variables_json.read_text()))
        evidence = static_evidence(
            parse_served_label_sets(json.loads(args.jobs_json.read_text()))
            if args.jobs_json else [])
    else:
        try:
            runners = fetch_runners(args.repo)
            variables = fetch_variables(args.repo)
        except FileNotFoundError:
            print(f"runner-topology: `{resolve_cli()}` not found; cannot read "
                  "live state.", file=sys.stderr)
            return 0 if args.mode == "hint" else 2
        except subprocess.CalledProcessError as exc:
            print(f"runner-topology: GitHub API call failed: "
                  f"{exc.stderr.strip()[:400]}", file=sys.stderr)
            return 0 if args.mode == "hint" else 2

        workflows_dir = args.workflows_dir or (HERE.parent.parent / ".github" / "workflows")

        def evidence(lane: Lane) -> list[set[str]]:
            # Called only when an ephemeral lane has no live runner, and scoped
            # to the workflows that actually consume the lane — a repo-wide
            # "last N runs" sweep is not a time window on a busy repo.
            consumers = find_consuming_workflows(lane.variable, workflows_dir)
            if not consumers:
                return []
            return fetch_served_label_sets(
                args.repo, contract.lookback_hours, consumers,
                contract.runs_per_workflow)

    findings = check(contract, runners, variables, evidence)

    if args.json:
        print(json.dumps([{
            "level": f.level, "kind": f.kind,
            "variable": f.variable, "detail": f.detail,
        } for f in findings], indent=2))
    else:
        print(render(findings))

    if args.mode == "hint":
        return 0
    return 1 if any(f.level == ERROR for f in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
