#!/usr/bin/env python3
"""Offline routing reachability and routing-override audit.

`runner_topology_check.py --mode=static` delegates here. Nothing in this module
touches the network: it answers "does DECLARED supply cover every lane's
DISPATCHED label set?" from three checked-in inputs.

    runner_topology.json          what each lane is contracted to request
    .github/workflows/*.yml       which workflow (by `name:`) dispatches it
    fleet_advertised_labels.json  what each runner registration advertises,
                                  generated from the fleet's checked-in
                                  profiles (schema tartci.advertised-labels/v1)

A job is dispatched to a registration only when all three hold: every label
the job requests is advertised by that registration (subset, case-insensitive,
GitHub's rule), the job's workflow name is one the registration mints runners
for, and the repository matches. Modelling only the first is how a lane reads
healthy while no provisioner ever picks its jobs up.

WHAT "REACHABLE" MEANS HERE
    Declared supply only. The snapshot is generated from profiles in source
    control, so a host whose INSTALLED profile has drifted from its checked-in
    one can read REACHABLE here while serving nothing live. That gap is the
    live checker's job (`--mode=report`); this mode exists so a routing edit
    can be judged on a PR with no network, no token, and no fleet access.

THE DISPATCHED LABEL SET IS NOT THE VARIABLE
    The required macOS gate's repo variable is a PRE-dispatch selector:
    build.yml removes the legacy shared label and appends one event-class
    label per event. Evaluating the raw variable would condemn the working
    gate, because no event-class registration advertises the removed label.
    The projection comes from `_event_projection`, the same function the live
    checker uses to reconcile the fleet profiles, so the two modes cannot
    disagree about what a gate job actually asks for.

ROUTING OVERRIDES
    A lane whose contracted value is a deliberate temporary state cites an
    entry in the contract's `overrides` array: who owns it, why, what would
    revert it, and when it expires. An expired override, an override naming a
    variable no lane or control declares, or a lane citing an override that
    does not exist is an error in every mode.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path
from typing import Any

REACHABLE = "REACHABLE"
UNSERVED = "UNSERVED"
HOSTED = "HOSTED"
SENTINEL = "SENTINEL"
UNKNOWN = "UNKNOWN"
UNDECLARED = "UNDECLARED"

SNAPSHOT_SCHEMA = "tartci.advertised-labels/v1"
OVERRIDE_FIELDS = ("id", "subject", "value", "owner", "reason",
                   "revert_condition", "since", "expires")

# build.yml maps a default workflow_dispatch (Shipyard's PR validation path)
# onto the PR-head class and drops the macOS leg entirely on `push`. Both are
# workflow facts the event-class contract does not carry, so they live here
# beside the one consumer that needs them.
DISPATCH_EVENT_ALIASES = {"workflow_dispatch": "pull_request"}

_SELECTOR_VARIABLE = re.compile(r"vars\.(PULP_[A-Z0-9_]*RUNS_ON_JSON)\b")
_WORKFLOW_NAME = re.compile(r"^name:\s*(.+?)\s*$", re.MULTILINE)
_WORKFLOW_CALL = re.compile(r"^\s+workflow_call\s*:", re.MULTILINE)


@dataclass
class Registration:
    profile: str
    host_id: str
    lane: str
    repo: str
    class_label: str | None
    labels: list[str]
    workflows: list[str]

    @property
    def folded(self) -> set[str]:
        return {label.lower() for label in self.labels}

    @property
    def handle(self) -> str:
        suffix = f"/{self.class_label}" if self.class_label else ""
        return f"{self.host_id}:{self.lane}{suffix}"


@dataclass
class Snapshot:
    registrations: list[Registration]
    generated_from: dict[str, Any] = field(default_factory=dict)


@dataclass
class Row:
    """One (lane, value, event, workflow) evaluation."""
    variable: str
    severity: str
    source: str          # "expect" or "unset_fallback"
    event: str           # "*" when the value is dispatched unchanged
    workflow: str | None
    labels: Any
    verdict: str
    detail: str
    # Host ids of the registrations that serve this row (REACHABLE only).
    served_by: list[str] = field(default_factory=list)

    @property
    def blocking(self) -> bool:
        return (self.verdict == UNSERVED and self.severity == "required"
                and self.source == "expect")


@dataclass
class OverrideStatus:
    id: str
    subject: str
    value: Any
    owner: str
    since: date | None
    expires: date | None
    age_days: int | None
    days_left: int | None


# ── Inputs ──────────────────────────────────────────────────────────────


def load_snapshot(path: Path) -> Snapshot:
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or data.get("schema") != SNAPSHOT_SCHEMA:
        raise ValueError(f"{path}: expected schema {SNAPSHOT_SCHEMA!r}")
    regs = []
    for raw in data.get("registrations", []):
        labels = raw.get("labels")
        workflows = raw.get("workflows")
        if (not isinstance(labels, list) or not labels
                or not isinstance(workflows, list)
                or not isinstance(raw.get("repo"), str)):
            raise ValueError(f"{path}: malformed registration {raw!r}")
        regs.append(Registration(
            profile=str(raw.get("profile", "")),
            host_id=str(raw.get("host_id", "")),
            lane=str(raw.get("lane", "")),
            repo=raw["repo"],
            class_label=raw.get("class_label"),
            labels=[str(label) for label in labels],
            workflows=[str(name) for name in workflows],
        ))
    return Snapshot(regs, data.get("generated_from") or {})


def consuming_workflows(variable: str, workflows_dir: Path) -> list[tuple[str, str | None]]:
    """(file, workflow name) for every workflow that references the variable.

    The name is None when it cannot be known statically: a reusable workflow
    (`workflow_call`) runs under its CALLER's name, so matching its own name
    against a registration's workflow list would be a guess.
    """
    needle = f"vars.{variable}"
    found: list[tuple[str, str | None]] = []
    if not workflows_dir.is_dir():
        return found
    paths = sorted(workflows_dir.glob("*.yml")) + sorted(workflows_dir.glob("*.yaml"))
    for path in paths:
        try:
            text = path.read_text()
        except OSError:
            continue
        if needle not in text:
            continue
        match = _WORKFLOW_NAME.search(text)
        name = match.group(1).strip("'\"") if match else None
        if _WORKFLOW_CALL.search(text):
            name = None
        found.append((path.name, name))
    return found


def referenced_variables(workflows_dir: Path) -> set[str]:
    names: set[str] = set()
    if not workflows_dir.is_dir():
        return names
    for path in list(workflows_dir.glob("*.yml")) + list(workflows_dir.glob("*.yaml")):
        try:
            names.update(_SELECTOR_VARIABLE.findall(path.read_text()))
        except OSError:
            continue
    return names


# ── Reachability ────────────────────────────────────────────────────────


def label_carriers(labels: list[str], repo: str, snapshot: Snapshot) -> list[Registration]:
    """Registrations advertising every requested label (subset, GitHub's rule)."""
    want = {label.lower() for label in labels}
    return [reg for reg in snapshot.registrations
            if reg.repo == repo and want <= reg.folded]


def serving(labels: list[str], workflow: str | None, repo: str,
            snapshot: Snapshot) -> list[Registration]:
    """Registrations that would pick up the job: labels, workflow, and repo."""
    if workflow is None:
        return []
    return [reg for reg in label_carriers(labels, repo, snapshot)
            if workflow in reg.workflows]


def serving_hosts(labels: list[str], workflow: str | None, repo: str,
                  snapshot: Snapshot) -> list[str]:
    return sorted({reg.host_id for reg in serving(labels, workflow, repo, snapshot)})


def reach(labels: list[str], workflow: str | None, repo: str,
          snapshot: Snapshot) -> tuple[str, str]:
    """Verdict for one dispatched label set from one workflow."""
    if workflow is None:
        return UNKNOWN, ("consuming workflow name is not statically known; "
                         "reachability depends on it, so it is not assumed")
    want = {label.lower() for label in labels}
    in_repo = [reg for reg in snapshot.registrations if reg.repo == repo]
    carriers = label_carriers(labels, repo, snapshot)
    served = serving(labels, workflow, repo, snapshot)
    if served:
        return REACHABLE, "by " + ", ".join(reg.handle for reg in served)
    if carriers:
        minted = sorted({name for reg in carriers for name in reg.workflows})
        return UNSERVED, (
            f"labels advertised by {', '.join(reg.handle for reg in carriers)} "
            f"but workflow {workflow!r} is not one they mint for {minted}")
    advertised = {label for reg in in_repo for label in reg.folded}
    missing = sorted(label for label in labels if label.lower() not in advertised)
    if missing:
        return UNSERVED, f"no {repo} registration advertises {missing}"
    best = min(in_repo, key=lambda reg: len(want - reg.folded), default=None)
    gap = sorted(want - best.folded) if best else sorted(want)
    return UNSERVED, (f"every label exists somewhere but no single registration "
                      f"carries them all (closest {best.handle if best else '-'} "
                      f"lacks {gap})")


def _classify(value: Any, contract: Any) -> str:
    # Imported lazily: runner_topology_check imports this module at load time.
    import runner_topology_check as rtc
    return rtc.classify_target(value, contract)


def _projections(lane: Any, contract: Any) -> tuple[list[tuple[str, list[str], str]] | None, str | None]:
    """(event, dispatched labels, workflow) triples for an event-class lane."""
    import runner_topology_check as rtc
    spec = contract.event_class_v2
    if not isinstance(spec, dict) or spec.get("variable") != lane.variable:
        return None, None
    projection, error = rtc._event_projection(contract)
    if projection is None:
        return None, error or "event_class_v2 projection unavailable"
    selector = projection["dynamic_selector"]
    removed = selector["legacy_label_removed_before_dispatch"]
    workflows = {row["event"]: row["workflow"] for row in spec["classes"]}
    events = dict(selector["event_labels"])
    for alias, target in DISPATCH_EVENT_ALIASES.items():
        events.setdefault(alias, events[target])
        workflows.setdefault(alias, workflows[target])
    out = []
    for event in sorted(events):
        labels = [label for label in selector["base_labels"] if label != removed]
        labels.append(events[event])
        out.append((event, labels, workflows[event]))
    return out, None


def evaluate_lane(lane: Any, contract: Any, snapshot: Snapshot,
                  workflows_dir: Path, repo: str) -> list[Row]:
    rows: list[Row] = []
    consumers = None

    def one(source: str, value: Any, project: bool) -> None:
        nonlocal consumers
        base = dict(variable=lane.variable, severity=lane.severity, source=source)
        if value is None:
            return
        kind = _classify(value, contract)
        if kind == "sentinel":
            rows.append(Row(**base, event="*", workflow=None, labels=value,
                            verdict=SENTINEL, detail="declared off-switch"))
            return
        if kind == "github-hosted":
            rows.append(Row(**base, event="*", workflow=None, labels=value,
                            verdict=HOSTED, detail="GitHub-hosted allowlist"))
            return
        if kind != "self-hosted":
            rows.append(Row(**base, event="*", workflow=None, labels=value,
                            verdict=UNSERVED,
                            detail="neither self-hosted nor in the hosted allowlist"))
            return
        supervisor = getattr(lane, "supervisor", None) or contract.static_default_supervisor
        if supervisor not in contract.static_covered_supervisors:
            rows.append(Row(**base, event="*", workflow=None, labels=value,
                            verdict=UNKNOWN,
                            detail=f"supply for supervisor {supervisor!r} is not "
                                   "in the advertised-labels snapshot"))
            return
        if project:
            projected, error = _projections(lane, contract)
            if error:
                rows.append(Row(**base, event="*", workflow=None, labels=value,
                                verdict=UNKNOWN, detail=error))
                return
            if projected is not None:
                for event, labels, workflow in projected:
                    verdict, detail = reach(labels, workflow, repo, snapshot)
                    rows.append(Row(**base, event=event, workflow=workflow,
                                    labels=labels, verdict=verdict, detail=detail,
                                    served_by=serving_hosts(labels, workflow, repo,
                                                            snapshot)))
                return
        if consumers is None:
            consumers = consuming_workflows(lane.variable, workflows_dir)
        if not consumers:
            rows.append(Row(**base, event="*", workflow=None, labels=value,
                            verdict=UNKNOWN, detail="no workflow references this variable"))
            return
        for _file, name in consumers:
            verdict, detail = reach(value, name, repo, snapshot)
            rows.append(Row(**base, event="*", workflow=name or _file,
                            labels=value, verdict=verdict, detail=detail,
                            served_by=serving_hosts(value, name, repo, snapshot)))

    one("expect", lane.expect, project=True)
    # The unset fallback is dispatched verbatim by the consuming workflow; no
    # event rewrite applies to a value the workflow's `||` supplies.
    one("unset_fallback", lane.unset_fallback, project=False)
    return rows


def evaluate(contract: Any, snapshot: Snapshot, workflows_dir: Path,
             repo: str) -> list[Row]:
    rows: list[Row] = []
    for lane in contract.lanes:
        rows.extend(evaluate_lane(lane, contract, snapshot, workflows_dir, repo))
    declared = {lane.variable for lane in contract.lanes}
    declared.update(contract.must_remain_unset)
    for name in sorted(referenced_variables(workflows_dir) - declared):
        rows.append(Row(name, "info", "expect", "*", None, None, UNDECLARED,
                        "referenced by a workflow but has no lane in runner_topology.json"))
    return rows


# ── Overrides ───────────────────────────────────────────────────────────


def _parse_date(value: Any) -> date | None:
    if not isinstance(value, str):
        return None
    try:
        return date.fromisoformat(value)
    except ValueError:
        return None


def check_overrides(raw_contract: dict[str, Any], contract: Any,
                    today: date) -> tuple[list[OverrideStatus], list[tuple[str, str, str]]]:
    """Return (active overrides, findings). A finding is (kind, subject, detail)."""
    overrides = raw_contract.get("overrides", [])
    findings: list[tuple[str, str, str]] = []
    statuses: list[OverrideStatus] = []
    if not isinstance(overrides, list):
        return [], [("override-invalid", "overrides", "must be an array")]
    subjects = {lane.variable: lane for lane in contract.lanes}
    controls = set(contract.routing_controls)
    by_id: dict[str, dict[str, Any]] = {}
    for raw in overrides:
        oid = raw.get("id") if isinstance(raw, dict) else None
        if not isinstance(oid, str) or not oid:
            findings.append(("override-invalid", "overrides", f"entry without an id: {raw!r}"))
            continue
        missing = [key for key in OVERRIDE_FIELDS if key not in raw
                   or raw[key] in (None, "")]
        if missing:
            findings.append(("override-invalid", oid, f"missing fields {missing}"))
        if oid in by_id:
            findings.append(("override-invalid", oid, "duplicate override id"))
        by_id[oid] = raw
        since, expires = _parse_date(raw.get("since")), _parse_date(raw.get("expires"))
        if since is None or expires is None:
            findings.append(("override-invalid", oid,
                             "since/expires must be ISO dates (YYYY-MM-DD)"))
        elif expires < today:
            findings.append(("override-expired", oid,
                             f"{raw.get('subject')} = {raw.get('value')!r} expired "
                             f"{expires.isoformat()} ({(today - expires).days} day(s) ago). "
                             f"Revert when: {raw.get('revert_condition')}. Either revert "
                             f"the lane or re-approve with a new expiry (owner "
                             f"{raw.get('owner')})."))
        subject = raw.get("subject")
        if subject not in subjects and subject not in controls:
            findings.append(("override-unknown-subject", oid,
                             f"subject {subject!r} is neither a lane variable nor a "
                             "routing control in runner_topology.json"))
        elif subject in subjects and subjects[subject].expect != raw.get("value"):
            findings.append(("override-stale", oid,
                             f"records {subject} = {raw.get('value')!r} but the lane "
                             f"now contracts {subjects[subject].expect!r}; retire the "
                             "override or update it"))
        elif subject in controls and contract.routing_controls[subject].expect != raw.get("value"):
            findings.append(("override-stale", oid,
                             f"records {subject} = {raw.get('value')!r} but the control "
                             f"contracts {contract.routing_controls[subject].expect!r}"))
        statuses.append(OverrideStatus(
            id=oid, subject=str(subject), value=raw.get("value"),
            owner=str(raw.get("owner", "")), since=since, expires=expires,
            age_days=(today - since).days if since else None,
            days_left=(expires - today).days if expires else None))
    for lane in contract.lanes:
        cited = getattr(lane, "override_id", None)
        if cited is None:
            continue
        target = by_id.get(cited)
        if target is None:
            findings.append(("override-missing", lane.variable,
                             f"cites override_id {cited!r}, which is not declared"))
        elif target.get("subject") != lane.variable:
            findings.append(("override-missing", lane.variable,
                             f"cites override_id {cited!r}, whose subject is "
                             f"{target.get('subject')!r}"))
    return statuses, findings


# ── Rendering ───────────────────────────────────────────────────────────


def _labels_text(labels: Any) -> str:
    if isinstance(labels, list):
        return ",".join(labels)
    return "" if labels is None else str(labels)


def render_table(rows: list[Row], snapshot: Snapshot, statuses: list[OverrideStatus],
                 override_findings: list[tuple[str, str, str]]) -> str:
    commit = (snapshot.generated_from.get("commit") or "unknown")[:12]
    out = [
        "runner-topology --mode=static: DECLARED supply "
        f"(advertised-labels snapshot @ {commit}), not live fleet state.",
        "",
        f"{'VARIABLE':42} {'SEV':8} {'SRC':8} {'EVENT':17} {'WORKFLOW':22} VERDICT     DETAIL",
    ]
    for row in rows:
        src = "fallback" if row.source == "unset_fallback" else "expect"
        out.append(f"{row.variable:42} {row.severity:8} {src:8} {row.event:17} "
                   f"{(row.workflow or '-'):22} {row.verdict:11} {row.detail}")
    out.append("")
    if statuses:
        out.append("Active routing overrides:")
        for st in statuses:
            out.append(f"  {st.id}: {st.subject} = {json.dumps(st.value)} "
                       f"(owner {st.owner}; since {st.since}, {st.age_days}d; "
                       f"expires {st.expires}, {st.days_left}d left)")
    else:
        out.append("Active routing overrides: none")
    for kind, subject, detail in override_findings:
        out.append(f"  ERROR [{kind}] {subject}: {detail}")
    blocking = [row for row in rows if row.blocking]
    fallback = [row for row in rows if row.verdict == UNSERVED and not row.blocking]
    out.append("")
    if blocking or override_findings:
        out.append(f"runner-topology static: FAIL — {len(blocking)} required lane "
                   f"row(s) UNSERVED, {len(override_findings)} override finding(s).")
    else:
        out.append("runner-topology static: OK — every required lane is REACHABLE, "
                   "HOSTED, or a SENTINEL on declared supply"
                   + (f" ({len(fallback)} advisory/fallback row(s) UNSERVED)."
                      if fallback else "."))
    return "\n".join(out)


def rows_json(rows: list[Row], statuses: list[OverrideStatus],
              override_findings: list[tuple[str, str, str]]) -> str:
    return json.dumps({
        "scope": "declared-supply",
        "rows": [{
            "variable": r.variable, "severity": r.severity, "source": r.source,
            "event": r.event, "workflow": r.workflow, "labels": r.labels,
            "verdict": r.verdict, "detail": r.detail, "blocking": r.blocking,
        } for r in rows],
        "overrides": [{
            "id": s.id, "subject": s.subject, "value": s.value, "owner": s.owner,
            "since": s.since.isoformat() if s.since else None,
            "expires": s.expires.isoformat() if s.expires else None,
            "age_days": s.age_days, "days_left": s.days_left,
        } for s in statuses],
        "override_findings": [{"kind": k, "subject": s, "detail": d}
                              for k, s, d in override_findings],
    }, indent=2)


def exit_code(rows: list[Row], override_findings: list[tuple[str, str, str]]) -> int:
    return 1 if any(row.blocking for row in rows) or override_findings else 0
