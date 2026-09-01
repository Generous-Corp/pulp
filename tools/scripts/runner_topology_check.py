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
    visibility-incomplete
                 — the labels matched nothing, but a runner scope REFUSED the
                   query, so the census never looked everywhere. Reported at
                   the lane's normal level, not below it: a real black hole on
                   an org-scoped lane is indistinguishable from this.
    degraded     — the only runners that match are offline (may just be asleep).
    hosted       — a GitHub-hosted scalar outside the allowlist (typo catch).
    must-unset   — a variable contracted to stay unset is set (cost guard).
    event-class  — optional TartCI source profile, installed receipt, and
                   private desired-fleet fixtures disagree with the Pulp v2
                   contract. These are read-only inputs; no host is queried.

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
    python3 tools/scripts/runner_topology_check.py \
        --fleet-profile fixtures/m3.toml --fleet-receipt fixtures/r.json \
        --fleet-source-manifest fixtures/fleet.json                    # read-only
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import workflow_runner_selector_audit  # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_CONTRACT = HERE / "runner_topology.json"
DEFAULT_REPO = "Generous-Corp/pulp"

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
    require_explicit_value: bool = False
    dispatch_only: bool = False

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
class RoutingControl:
    expect: str
    unset_fallback: str | None = None


@dataclass
class Contract:
    lanes: list[Lane]
    github_hosted_labels: set[str]
    sentinels: set[str]
    must_remain_unset: list[str]
    must_remain_unset_why: str
    routing_controls: dict[str, RoutingControl]
    lookback_hours: int
    runs_per_workflow: int
    event_class_v2: dict[str, Any] | None = None


@dataclass
class Finding:
    level: str
    kind: str
    variable: str
    detail: str


@dataclass
class RunnerInventory:
    """The runner census, plus an honest account of what it could not see.

    `unread_scopes` is load-bearing and separate from `warnings`: a scope that
    REFUSED the query is not a scope that answered "no runners". Adjudicating
    the two alike is how a check ends up asserting a fleet state it never
    observed, so the refusal travels as data rather than as prose.
    """
    runners: list[Runner] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    unread_scopes: list[str] = field(default_factory=list)


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
            require_explicit_value=bool(raw.get("require_explicit_value", False)),
            dispatch_only=bool(raw.get("dispatch_only", False)),
        )
        for raw in data.get("lanes", [])
    ]
    unset = data.get("must_remain_unset", {}) or {}
    evidence = data.get("service_evidence", {}) or {}
    controls = {}
    for name, raw in (data.get("routing_controls", {}) or {}).items():
        if isinstance(raw, str):
            controls[name] = RoutingControl(expect=raw)
        else:
            controls[name] = RoutingControl(
                expect=raw["expect"],
                unset_fallback=raw.get("unset_fallback"),
            )
    return Contract(
        lanes=lanes,
        github_hosted_labels=set(data.get("github_hosted_labels", [])),
        sentinels=set(data.get("sentinels", [])),
        must_remain_unset=list(unset.get("variables", [])),
        must_remain_unset_why=unset.get("why", ""),
        routing_controls=controls,
        lookback_hours=int(evidence.get("lookback_hours", 168)),
        runs_per_workflow=int(evidence.get("runs_per_workflow", 20)),
        event_class_v2=data.get("event_class_v2"),
    )


def load_toml_fixture(path: Path) -> dict[str, Any]:
    """Load optional profile evidence without raising the CLI's Python floor."""
    try:
        import tomllib
    except ImportError:
        try:
            import tomli as tomllib  # type: ignore[import-not-found,no-redef]
        except ImportError as exc:
            raise ValueError(
                "--fleet-profile requires Python 3.11+ or the optional tomli package"
            ) from exc
    return tomllib.loads(path.read_text())


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
    """Every runner that can serve this repo — repo-level AND org-level.

    The repo endpoint is structurally blind to runners registered in an ORG
    runner group, and Pulp's Linux capacity lives in exactly such a group. A
    repo-only query therefore reports zero Linux runners on a healthy fleet.
    This is not hypothetical: on 2026-08-16 two independent sessions read a
    repo-only result and concluded the Mac Pro Linux lane was dead. Neither
    query could have seen it either way.

    That blindness is worse here than in an ad-hoc probe, because this function
    feeds black-hole adjudication: a lane served entirely by org runners would
    be declared unroutable. Union both scopes so the verdict is about the
    fleet rather than about which endpoint was asked.

    An org query can legitimately fail on permissions. That degradation is
    surfaced, never swallowed — a silent fallback to repo-only would reproduce
    the exact failure this exists to prevent.
    """
    return fetch_runner_inventory(repo).runners


def fetch_runner_inventory(repo: str) -> RunnerInventory:
    """Census both scopes, and report which ones refused to answer."""
    warnings: list[str] = []
    unread_scopes: list[str] = []
    by_name: dict[str, Runner] = {}

    repo_payload = _api([f"repos/{repo}/actions/runners", "--paginate"])
    warnings.extend(_count_warnings(repo_payload, f"repos/{repo}"))
    for runner in parse_runners(repo_payload):
        by_name[runner.name] = runner

    org = repo.split("/", 1)[0]
    if org and "/" in repo:
        try:
            org_payload = _api([f"orgs/{org}/actions/runners", "--paginate"])
        except subprocess.CalledProcessError as exc:
            # Usually a token without org:admin. Say so: an operator who does
            # not know the org scope was skipped will read a partial inventory
            # as a complete one.
            detail = (exc.stderr or "").strip().splitlines()
            unread_scopes.append(f"orgs/{org}")
            warnings.append(
                f"org runner scope orgs/{org} was NOT read "
                f"({detail[-1][:160] if detail else 'unknown error'}); "
                "org-group runners are invisible to this run"
            )
        else:
            warnings.extend(_count_warnings(org_payload, f"orgs/{org}"))
            for runner in parse_runners(org_payload):
                by_name.setdefault(runner.name, runner)

    return RunnerInventory(
        runners=list(by_name.values()),
        warnings=warnings,
        unread_scopes=unread_scopes,
    )


def _count_warnings(payload: Any, scope: str) -> list[str]:
    """Flag a payload whose rendered rows disagree with its own total_count.

    A truncated or unpaginated response renders fewer rows than it reports, and
    an empty render alongside a non-zero `total_count` reads as "no runners"
    when it means "you did not receive them" — the misreading that cost a
    session on 2026-08-16.
    """
    if not isinstance(payload, dict):
        return []
    total = payload.get("total_count")
    rows = payload.get("runners")
    if not isinstance(total, int) or not isinstance(rows, list):
        return []
    if len(rows) != total:
        return [
            f"{scope} returned {len(rows)} runner rows but reports "
            f"total_count={total}; the inventory is incomplete"
        ]
    return []


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
    *,
    manual_only: bool = False,
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
        event_filter = "&event=workflow_dispatch" if manual_only else ""
        try:
            runs = _api([
                f"repos/{repo}/actions/workflows/{wf}/runs"
                f"?per_page={min(runs_per_workflow, 100)}{event_filter}",
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


# ── Event-class-v2 profile evidence ──


def _fleet_finding(kind: str, subject: str, detail: str) -> Finding:
    return Finding(ERROR, kind, subject, detail)


def _profile_lane(data: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any] | None:
    lanes = data.get("lane", [])
    if not isinstance(lanes, list):
        return None
    matches = [row for row in lanes if isinstance(row, dict)
               and row.get("id") == spec["lane_id"]
               and row.get("repo") == spec["repo"]]
    return matches[0] if len(matches) == 1 else None


def _event_projection(contract: Contract) -> tuple[dict[str, Any] | None, str | None]:
    """Canonical shared shape for code, TartCI profile, and fleet manifest."""
    spec = contract.event_class_v2
    if spec is None:
        return None, None
    if not isinstance(spec, dict):
        return None, "event_class_v2 must be an object"
    classes = spec.get("classes", [])
    omit_labels = spec.get("omit_labels", [])
    profiles = spec.get("profiles", [])
    if (not isinstance(classes, list)
            or not all(isinstance(row, dict)
                       and all(isinstance(row.get(key), str) and row[key]
                               for key in ("event", "label", "workflow"))
                       and type(row.get("runner_group_id")) is int
                       and type(row.get("lease_priority")) is int
                       for row in classes)
            or not isinstance(omit_labels, list)
            or not all(isinstance(label, str) and label for label in omit_labels)
            or not isinstance(profiles, list) or not profiles
            or not all(isinstance(row, dict)
                       and all(isinstance(row.get(key), str) and row[key]
                               for key in ("name", "manifest_host", "source_path"))
                       for row in profiles)
            or not all(isinstance(spec.get(key), str) and spec[key]
                       for key in ("variable", "lane_id", "repo",
                                   "assignment_mode"))):
        return None, "event_class_v2 has invalid typed fields"
    matching = [lane for lane in contract.lanes
                if lane.variable == spec.get("variable")]
    labels = [row.get("label") for row in classes]
    events = [row.get("event") for row in classes]
    group_ids = {row.get("runner_group_id") for row in classes}
    profile_names = [row["name"] for row in profiles]
    profile_hosts = [row["manifest_host"] for row in profiles]
    profile_sources = [row["source_path"] for row in profiles]
    if (len(matching) != 1 or not isinstance(matching[0].expect, list)
            or not all(isinstance(label, str) for label in matching[0].expect)):
        return None, "event_class_v2 must name one array-valued lane"
    if (len(events) != 2 or set(events) != {"merge_group", "pull_request"}
            or len(labels) != len(set(labels)) or len(group_ids) != 1
            or spec["assignment_mode"] != "event-class-v2"):
        return None, "event-class-v2 needs both events, unique labels, and one group"
    if any(len(values) != len(set(values)) for values in (
            profile_names, profile_hosts, profile_sources)):
        return None, "event-class-v2 profile declarations must be unique"
    if len(omit_labels) != 1:
        return None, "event-class-v2 requires exactly one legacy omit label"
    if any(label in matching[0].expect for label in labels):
        return None, "pre-dispatch selector already contains an event-class label"
    if any(label not in matching[0].expect for label in omit_labels):
        return None, "omit_labels contains a label absent from the base selector"
    tiers = [{key: row[key] for key in ("label", "workflow", "runner_group_id")}
             for row in classes]
    return {
        "dynamic_selector": {
            "base_labels": matching[0].expect,
            "event_labels": {row["event"]: row["label"] for row in classes},
            "legacy_label_removed_before_dispatch": omit_labels[0],
        },
        "lease_priority": {row["event"]: row["lease_priority"] for row in classes},
        "pulp_lane": {
            "assignment_mode": spec["assignment_mode"],
            "base_labels": [label for label in matching[0].expect
                            if label not in omit_labels],
            "omit_labels": omit_labels,
            "repo": spec["repo"],
            "runner_group_id": tiers[0]["runner_group_id"],
            "tiers": tiers,
        },
    }, None


def _project(data: Any, shape: dict[str, Any]) -> dict[str, Any]:
    """Select only typed fields Pulp owns; ignore provider-private additions."""
    if not isinstance(data, dict):
        return {}
    return {key: (_project(data.get(key, {}), value)
                  if isinstance(value, dict) else data.get(key))
            for key, value in shape.items()}


def _diff_paths(expected: Any, actual: Any, prefix: str = "") -> list[str]:
    if type(expected) is not type(actual):
        return [prefix]
    if isinstance(expected, dict):
        return [path for key, value in expected.items()
                for path in _diff_paths(value, actual.get(key),
                                        f"{prefix}.{key}" if prefix else key)]
    if isinstance(expected, list):
        if len(expected) != len(actual):
            return [prefix]
        return [path for index, value in enumerate(expected)
                for path in _diff_paths(value, actual[index],
                                        f"{prefix}[{index}]")]
    return [] if expected == actual else [prefix]


def _profile_projection(data: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any] | None:
    lane = _profile_lane(data, spec)
    if lane is None:
        return None
    tiers = lane.get("tier")
    return {
        "assignment_mode": lane.get("assignment_mode"),
        "base_labels": lane.get("labels"),
        "omit_labels": lane.get("assignment_omit_labels", []),
        "repo": lane.get("repo"),
        "runner_group_id": lane.get("runner_group_id"),
        "tiers": tiers,
    }


def check_event_class_evidence(
    contract: Contract,
    profile_inputs: list[tuple[Path, dict[str, Any]]] | None = None,
    receipt_inputs: list[tuple[Path, dict[str, Any]]] | None = None,
    source_manifest: tuple[Path, dict[str, Any]] | None = None,
) -> list[Finding]:
    """Reconcile optional, read-only TartCI fleet evidence.

    Inputs are fixtures: this never discovers hosts, reads launchd, or invokes
    TartCI. Pulp owns its repo-specific labels; TartCI remains a generic source
    of profile and install-receipt evidence.
    """
    spec = contract.event_class_v2
    if spec is None:
        return []
    expected, contract_error = _event_projection(contract)
    if contract_error:
        subject = (spec.get("variable", "event_class_v2")
                   if isinstance(spec, dict) else "event_class_v2")
        return [_fleet_finding(
            "event-class-contract", str(subject), contract_error)]
    assert expected is not None
    findings: list[Finding] = []
    declared_profiles = {row["name"]: row for row in spec.get("profiles", [])}
    profiles: dict[str, tuple[Path, dict[str, Any]]] = {}
    for path, data in profile_inputs or []:
        name = data.get("name") if isinstance(data, dict) else None
        subject = str(name or path)
        declared = declared_profiles.get(name) if isinstance(name, str) else None
        lane = _profile_lane(data, spec) if isinstance(data, dict) else None
        profile_view = _profile_projection(data, spec) if lane else None
        tiers = lane.get("tier", []) if lane else []
        fixed_priority = bool(lane and ("priority" in lane or (
            isinstance(tiers, list) and any(
            isinstance(row, dict) and "priority" in row
            for row in tiers))))
        paths = (_diff_paths(expected["pulp_lane"], profile_view)
                 if profile_view is not None else ["pulp_lane"])
        if (not isinstance(data, dict) or type(data.get("schema")) is not int
                or data.get("schema") != 1):
            paths.append("schema")
        host = data.get("host", {}) if isinstance(data, dict) else {}
        if not isinstance(host, dict):
            host = {}
        for key in ("id", "tart_home"):
            if not isinstance(host.get(key), str) or not host[key].strip():
                paths.append(f"host.{key}")
        if declared is not None:
            declared_parts = Path(declared["source_path"]).parts
            if path.parts[-len(declared_parts):] != declared_parts:
                paths.append("source_path")
        duplicate = isinstance(name, str) and name in profiles
        if declared is None or duplicate or fixed_priority or paths:
            reasons = paths
            if declared is None:
                reasons.append("profile declaration")
            if duplicate:
                reasons.append("duplicate profile")
            if fixed_priority:
                reasons.append("fixed lane priority")
            findings.append(_fleet_finding(
                "profile-contract-drift", subject,
                "TartCI source profile differs at: " + ", ".join(reasons),
            ))
        if isinstance(name, str) and name not in profiles:
            profiles[name] = (path, data)

    seen_receipts: set[str] = set()
    for path, receipt in receipt_inputs or []:
        profile_name = receipt.get("profile") if isinstance(receipt, dict) else None
        subject = str(profile_name or path)
        source = profiles.get(profile_name) if isinstance(profile_name, str) else None
        digest = hashlib.sha256(source[0].read_bytes()).hexdigest() if source else None
        installed_path = receipt.get("config_path") if isinstance(receipt, dict) else None
        # TartCI receipts name the installed host snapshot
        # (~/.config/tartci/macos-fleet-profile.toml), not the checked-in
        # profiles/<host>.toml source. Their exact-byte digest is the portable
        # cross-root binding; comparing those intentionally different paths
        # would reject every genuine installation receipt.
        if (not isinstance(receipt, dict)
                or type(receipt.get("schema")) is not int
                or receipt.get("schema") != 2
                or not isinstance(profile_name, str) or source is None
                or profile_name in seen_receipts
                or not isinstance(installed_path, str)
                or not Path(installed_path).is_absolute()
                or receipt.get("config_sha256") != digest):
            findings.append(_fleet_finding(
                "profile-receipt-drift", subject,
                "receipt schema/profile/digest does not bind one exact supplied "
                f"source profile (source sha256={digest!r}).",
            ))
        if isinstance(profile_name, str):
            seen_receipts.add(profile_name)

    if source_manifest is None:
        return findings

    manifest_path, manifest = source_manifest
    if not isinstance(manifest, dict):
        findings.append(_fleet_finding(
            "source-manifest-drift", str(manifest_path),
            "private fleet source manifest must be a JSON object.",
        ))
        return findings
    mismatches = _diff_paths(expected, _project(manifest, expected))
    source_paths = manifest.get("source_paths", {})
    if not isinstance(source_paths, dict):
        mismatches.append("source_paths")
        source_paths = {}
    if type(manifest.get("schema")) is not int or manifest.get("schema") != 1:
        mismatches.append("schema")
    topology_source = source_paths.get("pulp_topology")
    if topology_source != "tools/scripts/runner_topology.json":
        mismatches.append("source_paths.pulp_topology")

    manifest_profiles = source_paths.get("tartci_profiles", [])
    if not isinstance(manifest_profiles, list):
        manifest_profiles = []
        mismatches.append("source_paths.tartci_profiles")
    hosts = manifest.get("hosts", {})
    if not isinstance(hosts, dict):
        mismatches.append("hosts")
        hosts = {}
    for name, declared in declared_profiles.items():
        if declared["source_path"] not in manifest_profiles:
            mismatches.append("source_paths.tartci_profiles")
        manifest_host = hosts.get(declared["manifest_host"], {})
        if not isinstance(manifest_host, dict):
            mismatches.append(f"hosts.{declared['manifest_host']}")
            manifest_host = {}
        for key in ("host_id", "tart_home"):
            if (not isinstance(manifest_host.get(key), str)
                    or not manifest_host[key].strip()):
                mismatches.append(f"hosts.{declared['manifest_host']}.{key}")
        supplied = profiles.get(name)
        if supplied is None:
            continue
        _profile_path, profile_data = supplied
        profile_host = profile_data.get("host", {})
        if not isinstance(profile_host, dict):
            profile_host = {}
        lane_data = _profile_lane(profile_data, spec)
        host_expected = {"host_id": profile_host.get("id"),
                         "tart_home": profile_host.get("tart_home")}
        for key in ("min_queued_age_seconds", "supervisors", "vm_cores"):
            if lane_data and key in manifest_host:
                host_expected[key] = lane_data.get(key)
        mismatches.extend(_diff_paths(
            host_expected,
            manifest_host,
            f"hosts.{declared['manifest_host']}"))
    if mismatches:
        findings.append(_fleet_finding(
            "source-manifest-drift", str(manifest_path),
            "private fleet source manifest differs at: "
            + ", ".join(sorted(set(mismatches))),
        ))
    return findings


# ── Checks ──────────────────────────────────────────────────────────────


def _level_for(lane: Lane) -> str:
    return ERROR if lane.severity == "required" else WARN


def static_evidence(served: list[set[str]]):
    """An evidence provider backed by a fixed list (fixtures, tests)."""
    return lambda _lane: served


def check_selector_parsing(
    contract: Contract, workflows_dir: Path
) -> list[Finding]:
    """Would setting a contracted lane's variable actually route, or hang?

    Every other check here asks whether capacity exists. This one asks whether
    the workflow can express the answer at all. A `runs-on` that interpolates a
    lane variable without `fromJSON` collapses the JSON array into ONE literal
    label; no runner carries it, and GitHub queues the job forever rather than
    failing it. Capacity is irrelevant — the live incident on 2026-08-16 used a
    HOSTED value and still hung a required check repo-wide.

    This tool was green throughout that window because attribution is a
    substring search: it answers "which workflows mention this variable", which
    is a different question and cannot distinguish a parsed reference from an
    interpolated one. The gap is narrow and specific, so this is additive — no
    existing adjudication changes.
    """
    findings: list[Finding] = []
    if not workflows_dir.is_dir():
        return findings
    offenders = workflow_runner_selector_audit.audit_directory(workflows_dir)
    if not offenders:
        return findings
    declared = {lane.variable for lane in contract.lanes}
    for offender in offenders:
        # Report undeclared variables too. A lane absent from the contract is
        # exactly the one nobody is watching, so staying silent about it would
        # reproduce the failure this check exists for.
        scope = "" if offender.variable in declared else " (not in the contract)"
        findings.append(Finding(
            ERROR, "unparsed-selector", offender.variable,
            f"{offender.workflow}:{offender.line} interpolates this variable "
            f"into `runs-on` without fromJSON{scope}. Setting it would yield "
            "one literal JSON-array label that no runner carries, and GitHub "
            "queues such a job forever instead of failing it.",
        ))
    return findings


def check(
    contract: Contract,
    runners: list[Runner],
    variables: dict[str, str],
    evidence: Any,
    workflows_dir: Path | None = None,
    unread_scopes: list[str] | None = None,
) -> list[Finding]:
    """`evidence` is a callable lane -> list of served label sets, invoked ONLY
    when an ephemeral lane has no live runner. A list is accepted for
    convenience and wrapped. Laziness matters: the scan costs API calls, and a
    healthy fleet must not pay for them every sweep.

    `workflows_dir` enables the selector-parsing check. It is optional so that
    offline fixture runs, which have no workflow tree, keep working unchanged.

    `unread_scopes` names the runner scopes that refused the census (see
    `RunnerInventory`). It downgrades no verdict — it only replaces the claim
    "nothing serves this lane" with the claim the evidence actually supports.
    """
    if not callable(evidence):
        evidence = static_evidence(evidence)
    findings = check_event_class_evidence(contract)
    declared = {lane.variable for lane in contract.lanes}

    if workflows_dir is not None:
        findings.extend(check_selector_parsing(contract, workflows_dir))

    # Cost guard: a paid-overflow variable that is set at all.
    for name in contract.must_remain_unset:
        if name in variables:
            findings.append(Finding(
                ERROR, "must-unset", name,
                f"contracted to stay UNSET but is set to {variables[name]!r}. "
                f"{contract.must_remain_unset_why}",
            ))

    # Non-selector controls can still invalidate selector behavior. Keep their
    # exact live values reviewed beside the lanes they influence.
    for name, control in contract.routing_controls.items():
        actual = variables.get(name)
        effective = actual if actual is not None else control.unset_fallback
        if effective != control.expect:
            findings.append(Finding(
                ERROR, "control-drift", name,
                f"live value {actual!r} (effective {effective!r}) != "
                f"contracted {control.expect!r}. "
                "Reconcile the routing control and topology contract together.",
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
        findings.extend(_check_lane(lane, contract, runners, variables, evidence,
                                    unread_scopes or []))

    return findings


def _check_lane(
    lane: Lane,
    contract: Contract,
    runners: list[Runner],
    variables: dict[str, str],
    evidence: Any,
    unread_scopes: list[str],
) -> list[Finding]:
    findings: list[Finding] = []

    if lane.variable not in variables:
        if lane.require_explicit_value:
            findings.append(Finding(
                _level_for(lane), "unset", lane.variable,
                "contract requires an explicit value, but the variable is not "
                f"set. Unset would activate {lane.unset_fallback!r}. "
                f"Purpose: {lane.purpose}",
            ))
            return findings
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
                             origin="workflow fallback (variable unset)",
                             unread_scopes=unread_scopes)

    # ── drift: the variable must match what the contract says it routes to.
    actual_raw = variables[lane.variable]

    # A sentinel is a bare word by design, not a JSON array — `local-only` is
    # the documented off switch for macOS overflow, because emptiness cannot
    # disable a lane whose workflow `||` default would just re-fill it. It has
    # to be recognized BEFORE the parse: json.loads() rejects it, so the lane's
    # deliberate off state was being reported as a malformed value that would
    # "fail at dispatch." A standing error for an intended state is worse than
    # no check, because it teaches everyone to skim past this report.
    if actual_raw in contract.sentinels:
        if actual_raw != lane.expect:
            findings.append(Finding(
                ERROR, "drift", lane.variable,
                f"live sentinel {actual_raw!r} != contracted "
                f"{lane.expect!r}. Reconcile both together.",
            ))
        return findings

    try:
        actual = json.loads(actual_raw)
    except json.JSONDecodeError:
        findings.append(Finding(
            ERROR, "malformed", lane.variable,
            f"value {actual_raw!r} is not valid JSON; `fromJSON()` in the "
            "workflow will fail at dispatch.",
        ))
        return findings

    # Sentinel contracts are representation-sensitive. The workflow compares
    # the raw value before `fromJSON`; JSON-quoted `"local-only"` is therefore
    # a runner selector, not the off switch, even though json.loads produces
    # the same Python string. A different real selector is ordinary drift and
    # must continue through black-hole adjudication below.
    if (
        isinstance(lane.expect, str)
        and lane.expect in contract.sentinels
        and actual == lane.expect
    ):
        findings.append(Finding(
            ERROR, "sentinel-encoding", lane.variable,
            f"contracted sentinel {lane.expect!r} must be stored as that exact "
            f"bare value; live raw value is {actual_raw!r}.",
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
                                  origin="variable", unread_scopes=unread_scopes))
    return findings


def _check_target(
    lane: Lane,
    contract: Contract,
    runners: list[Runner],
    evidence: Any,
    target: Any,
    origin: str,
    unread_scopes: list[str] | None = None,
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

    if lane.dispatch_only:
        if has_service_evidence(target, evidence(lane)):
            findings.append(Finding(
                OK, "dispatch-only-idle", lane.variable,
                f"{labels_str} has no live runner, but a matching manual job "
                "was served within the lookback window.",
            ))
            return findings
        findings.append(Finding(
            _level_for(lane), "dispatch-only-unverified", lane.variable,
            f"{labels_str} has no live runner. This lane is operator-dispatched, "
            f"but no matching job was served in the last "
            f"{contract.lookback_hours}h; verify its provisioner before dispatch.",
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
        if unread_scopes:
            findings.append(_visibility_incomplete(
                lane, labels_str, unread_scopes,
                f"It has also served no job in the last {contract.lookback_hours}h, "
                "which is what an idle provisioner behind an unreadable scope "
                "looks like too.",
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

    if unread_scopes:
        findings.append(_visibility_incomplete(
            lane, labels_str, unread_scopes,
            "A runner registered in that scope would carry these labels and "
            "never appear here.",
        ))
        return findings

    findings.append(Finding(
        _level_for(lane), "black-hole", lane.variable,
        f"{labels_str} is carried by NO registered runner (online or offline). "
        "Jobs routed here queue forever with no error. "
        f"Purpose: {lane.purpose}",
    ))
    return findings


def _visibility_incomplete(
    lane: Lane,
    labels_str: str,
    unread_scopes: list[str],
    census_note: str,
) -> Finding:
    """The lane matched nothing, but the census did not read every scope.

    Emitted at the lane's NORMAL level, never below it. Downgrading it would
    make a persistent token-scope regression report green every hour and
    auto-close its own tracking issue, hiding a genuinely dead lane behind a
    permissions bug — the exact substitution of "unobserved" for "healthy"
    that the union of scopes exists to prevent.
    """
    scopes = ", ".join(unread_scopes)
    return Finding(
        _level_for(lane), "visibility-incomplete", lane.variable,
        f"{labels_str} is carried by no runner the census could see — but the "
        f"census is INCOMPLETE: {scopes} refused the query, so runners "
        f"registered there were never read. {census_note} "
        "This is NOT downgraded to a warning, because a genuine black hole on "
        "an org-scoped lane looks exactly like this. Restore the token's "
        f"`Administration: Read` scope for {scopes}, or verify this lane's "
        "provisioner manually, before treating the lane as either healthy or "
        f"dead. Purpose: {lane.purpose}",
    )


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
        if any(f.kind == "visibility-incomplete" for f in findings):
            # This summary is what the tracking issue shows first, so it must
            # not send an operator to reroute a lane when the actual defect is
            # that the census could not read a runner scope.
            lines.append("")
            lines.append("SOME LANES ABOVE WERE NOT FULLY OBSERVED: a runner scope refused")
            lines.append("the census, so their labels matched nothing that could be read.")
            lines.append("Repair the token scope before rerouting anything — until then")
            lines.append("neither 'healthy' nor 'dead' is established for those lanes.")
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
    ap.add_argument("--fleet-profile", type=Path, action="append", default=[],
                    help="Read-only TartCI source profile fixture (repeatable).")
    ap.add_argument("--fleet-receipt", type=Path, action="append", default=[],
                    help="Read-only installed-profile receipt fixture (repeatable).")
    ap.add_argument("--fleet-source-manifest", type=Path,
                    help="Read-only private desired-fleet manifest fixture.")
    ap.add_argument("--json", action="store_true", help="Emit findings as JSON.")
    args = ap.parse_args(argv)

    try:
        contract = load_contract(args.contract)
        profile_inputs = [
            (path, load_toml_fixture(path)) for path in args.fleet_profile
        ]
        receipt_inputs = [
            (path, json.loads(path.read_text())) for path in args.fleet_receipt
        ]
        source_manifest = (
            (args.fleet_source_manifest,
             json.loads(args.fleet_source_manifest.read_text()))
            if args.fleet_source_manifest else None
        )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"runner-topology: evidence input failed: {exc}", file=sys.stderr)
        return 0 if args.mode == "hint" else 2
    workflows_dir = args.workflows_dir or (
        HERE.parent.parent / ".github" / "workflows"
    )

    offline_inputs = bool(args.runners_json and args.variables_json)
    unread_scopes: list[str] = []
    if offline_inputs:
        runners = parse_runners(json.loads(args.runners_json.read_text()))
        variables = parse_variables(json.loads(args.variables_json.read_text()))
        evidence = static_evidence(
            parse_served_label_sets(json.loads(args.jobs_json.read_text()))
            if args.jobs_json else [])
    else:
        try:
            inventory = fetch_runner_inventory(args.repo)
            runners = inventory.runners
            unread_scopes = inventory.unread_scopes
            for warning in inventory.warnings:
                print(f"runner-topology: WARNING: {warning}", file=sys.stderr)
            variables = fetch_variables(args.repo)
        except FileNotFoundError:
            print(f"runner-topology: `{resolve_cli()}` not found; cannot read "
                  "live state.", file=sys.stderr)
            return 0 if args.mode == "hint" else 2
        except subprocess.CalledProcessError as exc:
            print(f"runner-topology: GitHub API call failed: "
                  f"{exc.stderr.strip()[:400]}", file=sys.stderr)
            return 0 if args.mode == "hint" else 2

        def evidence(lane: Lane) -> list[set[str]]:
            # Called only when an ephemeral lane has no live runner, and scoped
            # to the workflows that actually consume the lane — a repo-wide
            # "last N runs" sweep is not a time window on a busy repo.
            consumers = find_consuming_workflows(lane.variable, workflows_dir)
            if not consumers:
                return []
            return fetch_served_label_sets(
                args.repo, contract.lookback_hours, consumers,
                contract.runs_per_workflow, manual_only=lane.dispatch_only)

    findings = check(contract, runners, variables, evidence, workflows_dir,
                     unread_scopes)
    if profile_inputs or receipt_inputs or source_manifest:
        findings.extend(
            finding for finding in check_event_class_evidence(
                contract, profile_inputs, receipt_inputs, source_manifest)
            if finding not in findings
        )

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
