#!/usr/bin/env python3
"""Closed semantic validator for GPU first-visible A3 acceptance v2."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import re
import sys
from pathlib import Path
from typing import Any, Callable

import json_schema_lite


SCHEMA_PATH = (
    Path(__file__).resolve().parents[2]
    / "docs/contracts/gpu-first-visible-a3-acceptance-v2.schema.json"
)
ROLE_SPECS = (
    ("pulp-standalone", "pulp-standalone", "pulp-standalone-canary", "standalone", "native-compositor-presentation"),
    ("forge-modular-standalone", "forge-modular-standalone", "forge-modular", "standalone", "native-compositor-presentation"),
    ("forge-modular-auv2-logic", "logic-pro", "forge-modular", "auv2", "native-compositor-presentation"),
    ("forge-modular-vst3-reaper", "reaper", "forge-modular", "vst3", "native-compositor-presentation"),
    ("forge-modular-clap-reaper", "reaper", "forge-modular", "clap", "native-compositor-presentation"),
    ("headless-reference", "headless-capture", "pulp-standalone-canary", "headless", "headless-capture-complete"),
    ("constrained-adapter", "pulp-standalone", "pulp-standalone-canary", "standalone", "native-compositor-presentation"),
)
ROLE_IDS = tuple(row[0] for row in ROLE_SPECS)
ROLE_BY_ID = {row[0]: row for row in ROLE_SPECS}
VISIBLE_ROLES = frozenset(set(ROLE_IDS) - {"headless-reference"})
FORGE_ROLES = frozenset(role for role in ROLE_IDS if role.startswith("forge-modular-"))
STATES = (
    "baseline-tracing-off",
    "candidate-tracing-off",
    "candidate-compiled-in-idle",
    "candidate-active-session",
)
TRACE_CATEGORIES = ("dsp", "gpu", "metadata", "render", "js", "state")
A4_SCENARIOS = (
    "dense-text-thin-strokes", "shader-heavy-controls", "meters-waveforms",
    "threejs-audio-reactive", "forge-modular-native", "forge-modular-daw",
    "super-convolver-web",
)
BLOCKERS = ("product-policy", "required-coverage")
POLICY_REPOSITORIES = {
    "Generous-Corp/pulp", "Generous-Corp/forge", "danielraffel/pulp-planning",
}
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
ARTIFACT_KEYS = {"path", "sha256"}


class V2AcceptanceError(ValueError):
    """A v2 receipt cannot substantiate its declared state."""


def exact_keys(value: Any, expected: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        raise V2AcceptanceError(f"{label} has the wrong fields")
    return value


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise V2AcceptanceError(f"cannot read JSON artifact {path}: {error}") from error


def expand_local_schema_refs(value: Any, root: dict[str, Any]) -> Any:
    """Expand the schema's closed local ``$defs`` references for the lite gate."""
    if isinstance(value, list):
        return [expand_local_schema_refs(item, root) for item in value]
    if not isinstance(value, dict):
        return value
    if set(value) == {"$ref"}:
        reference = value["$ref"]
        prefix = "#/$defs/"
        if not isinstance(reference, str) or not reference.startswith(prefix):
            raise V2AcceptanceError(f"unsupported A3 v2 schema reference: {reference!r}")
        name = reference[len(prefix):]
        definitions = root.get("$defs")
        if not isinstance(definitions, dict) or name not in definitions:
            raise V2AcceptanceError(f"unknown A3 v2 schema definition: {name}")
        return expand_local_schema_refs(definitions[name], root)
    return {
        key: expand_local_schema_refs(item, root)
        for key, item in value.items()
        if key != "$defs"
    }


def resolve_artifact(
    ref: Any, evidence_root: Path, label: str,
    *, loader: Callable[[Path], Any] = load_json,
) -> Any:
    exact_keys(ref, ARTIFACT_KEYS, label)
    relative = Path(ref["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise V2AcceptanceError(f"{label}.path must be safe and relative")
    path = evidence_root / relative
    try:
        data = path.read_bytes()
    except OSError as error:
        raise V2AcceptanceError(f"cannot read {label}: {error}") from error
    if hashlib.sha256(data).hexdigest() != ref["sha256"]:
        raise V2AcceptanceError(f"{label} digest mismatch")
    return loader(path)


def canonical_protocol() -> dict[str, Any]:
    return {
        "clock": "mach_continuous_time",
        "reference_host_id": "m5",
        "ring_mib": 128,
        "timer_noise_pairs": 10000,
        "warmup_trials_per_state": 5,
        "warm_trials_per_state": 30,
        "cold_trials_per_state": 20,
        "states": list(STATES),
        "trace_categories": list(TRACE_CATEGORIES),
        "roles": [
            {
                "role_id": role,
                "host_kind": host,
                "application_kind": application,
                "plugin_format": plugin_format,
                "measurement_endpoint": endpoint,
            }
            for role, host, application, plugin_format, endpoint in ROLE_SPECS
        ],
    }


def derived_seed(policy_sha256: str, role_id: str) -> int:
    digest = hashlib.sha256(f"{policy_sha256}:{role_id}:a3-v2".encode()).digest()
    return int.from_bytes(digest[:8], "big")


def derived_trial_order(policy_sha256: str, role_id: str) -> list[str]:
    tokens = [
        f"{state}:{kind}:{index}"
        for state in STATES
        for kind, count in (("warmup", 5), ("warm", 30), ("cold", 20))
        for index in range(count)
    ]
    random.Random(derived_seed(policy_sha256, role_id)).shuffle(tokens)
    return tokens


def percentile(values: list[int], percent: int) -> int:
    if not values:
        raise V2AcceptanceError("cannot derive a percentile from zero samples")
    return sorted(values)[math.ceil(len(values) * percent / 100) - 1]


def _positive_int(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise V2AcceptanceError(f"{label} must be a positive integer")
    return value


def validate_product_policy(
    receipt: dict[str, Any], evidence_root: Path,
) -> tuple[dict[str, Any], str, set[str]]:
    binding = receipt["product_policy"]
    if binding["status"] != "bound" or binding["required_coverage"] != "bound":
        raise V2AcceptanceError("complete receipt requires bound product policy and required coverage")
    policy = resolve_artifact(binding["authority"], evidence_root, "product_policy.authority")
    validation = resolve_artifact(
        binding["validation"], evidence_root, "product_policy.validation"
    )
    exact_keys(policy, {
        "schema", "version", "budget_id", "source", "approval", "clock",
        "editor_open_origin", "first_nonblank_endpoint", "statistic",
        "first_applicable_pulp_revision", "first_applicable_forge_revision",
        "reference_host", "roles", "required_coverage", "a4_scenario_budgets",
        "canary",
    }, "product policy")
    if (
        policy["schema"] != "pulp.gpu-first-visible-budget-authority.v1"
        or policy["version"] != 1
        or policy["clock"] != "mach_continuous_time"
        or policy["editor_open_origin"] != "editor-open-requested"
        or policy["first_nonblank_endpoint"] != "first-nonblank-presented-frame"
        or policy["statistic"] != "p95"
    ):
        raise V2AcceptanceError("product policy has the wrong closed measurement semantics")
    source = exact_keys(
        policy["source"], {"repository", "revision", "path", "blob"},
        "product policy source",
    )
    if (
        source["repository"] not in POLICY_REPOSITORIES
        or not GIT_SHA.fullmatch(source["revision"])
        or not GIT_SHA.fullmatch(source["blob"])
        or not isinstance(source["path"], str)
        or not source["path"]
    ):
        raise V2AcceptanceError("product policy source authority is invalid")
    approval = exact_keys(
        policy["approval"],
        {"github_user_id", "mode", "pr_url", "approved_head"},
        "product policy approval",
    )
    if (
        approval["github_user_id"] != 25807
        or approval["mode"] not in {"author", "approval"}
        or not isinstance(approval["pr_url"], str)
        or not approval["pr_url"].startswith("https://github.com/")
        or approval["approved_head"] != source["revision"]
    ):
        raise V2AcceptanceError("product policy is not bound to Daniel's immutable approval")
    exact_keys(validation, {
        "schema", "version", "status", "checked_at", "fresh_live_state",
        "protected_commit", "repository", "revision", "path", "blob",
        "github_user_id", "approved_head",
    }, "product policy validation")
    if (
        validation["schema"] != "pulp.gpu-first-visible-product-policy-validation.v1"
        or validation["version"] != 1
        or validation["status"] != "pass"
        or validation["fresh_live_state"] is not True
        or validation["protected_commit"] is not True
        or validation["github_user_id"] != 25807
        or validation["repository"] != source["repository"]
        or validation["revision"] != source["revision"]
        or validation["path"] != source["path"]
        or validation["blob"] != source["blob"]
        or validation["approved_head"] != approval["approved_head"]
    ):
        raise V2AcceptanceError("product policy validation does not prove fresh protected authority")
    host = exact_keys(
        policy["reference_host"],
        {"host_id", "machine_id", "hardware", "os", "display", "refresh_hz"},
        "product policy reference host",
    )
    if host["host_id"] != "m5" or not isinstance(host["refresh_hz"], (int, float)) or host["refresh_hz"] <= 0:
        raise V2AcceptanceError("product policy must bind the exact m5 reference host")
    for field in ("first_applicable_pulp_revision", "first_applicable_forge_revision"):
        if not isinstance(policy[field], str) or not GIT_SHA.fullmatch(policy[field]):
            raise V2AcceptanceError(f"product policy {field} must be an exact revision")
    role_rows = policy["roles"]
    if not isinstance(role_rows, list) or [row.get("role_id") for row in role_rows if isinstance(row, dict)] != list(ROLE_IDS):
        raise V2AcceptanceError("product policy must contain the exact seven ordered role IDs")
    for row in role_rows:
        exact_keys(row, {
            "role_id", "first_visible_p95_ns", "first_interaction",
            "steady_cpu_frame_p95_ns", "steady_gpu_frame_p95_ns",
        }, f"product policy role {row.get('role_id')}")
        _positive_int(row["first_visible_p95_ns"], "first-visible threshold")
        _positive_int(row["steady_cpu_frame_p95_ns"], "steady CPU threshold")
        _positive_int(row["steady_gpu_frame_p95_ns"], "steady GPU threshold")
        interaction = exact_keys(
            row["first_interaction"],
            {"status", "origin", "stimulus", "expected_state_change", "endpoint", "p95_ns"},
            f"product policy role {row['role_id']} interaction",
        )
        if row["role_id"] == "headless-reference":
            if interaction != {
                "status": "not-applicable-by-authority", "origin": None,
                "stimulus": None, "expected_state_change": None,
                "endpoint": None, "p95_ns": None,
            }:
                raise V2AcceptanceError("headless-reference interaction must be not-applicable-by-authority")
        else:
            if interaction["status"] != "measured":
                raise V2AcceptanceError(f"{row['role_id']} lacks measured interaction authority")
            for field in ("origin", "stimulus", "expected_state_change", "endpoint"):
                if not isinstance(interaction[field], str) or not interaction[field]:
                    raise V2AcceptanceError(f"{row['role_id']} interaction.{field} is missing")
            _positive_int(interaction["p95_ns"], "first-interaction threshold")
    coverage = exact_keys(
        policy["required_coverage"],
        {"predicate", "adapter", "configuration", "support_matrix", "a1_evidence"},
        "product policy required coverage",
    )
    for field in ("predicate", "adapter", "configuration"):
        if not isinstance(coverage[field], str) or not coverage[field]:
            raise V2AcceptanceError(f"required coverage {field} is missing")
    exact_keys(coverage["support_matrix"], ARTIFACT_KEYS, "required coverage support matrix")
    if not isinstance(coverage["a1_evidence"], list) or not coverage["a1_evidence"]:
        raise V2AcceptanceError("required coverage lacks authentic A1 evidence")
    for index, ref in enumerate(coverage["a1_evidence"]):
        exact_keys(ref, ARTIFACT_KEYS, f"required coverage A1 evidence {index}")
    nested_digests = {coverage["support_matrix"]["sha256"]}
    resolve_artifact(
        coverage["support_matrix"], evidence_root,
        "required coverage support matrix", loader=lambda path: path.read_bytes(),
    )
    for index, ref in enumerate(coverage["a1_evidence"]):
        resolve_artifact(
            ref, evidence_root, f"required coverage A1 evidence {index}",
            loader=lambda path: path.read_bytes(),
        )
        nested_digests.add(ref["sha256"])
    canary = exact_keys(policy["canary"], {
        "binary_sha256", "content_sha256", "signature_sha256",
        "editor_open_origin", "interaction_lifecycle", "steady_state_workload",
    }, "product policy canary")
    for field in ("binary_sha256", "content_sha256", "signature_sha256"):
        if not isinstance(canary[field], str) or not SHA256.fullmatch(canary[field]):
            raise V2AcceptanceError(f"product policy canary.{field} is invalid")
    for field in ("editor_open_origin", "interaction_lifecycle", "steady_state_workload"):
        if not isinstance(canary[field], str) or not canary[field]:
            raise V2AcceptanceError(f"product policy canary.{field} is missing")
    scenarios = policy["a4_scenario_budgets"]
    if not isinstance(scenarios, list) or [
        row.get("scenario_id") for row in scenarios if isinstance(row, dict)
    ] != list(A4_SCENARIOS):
        raise V2AcceptanceError("product policy lacks the exact seven ordered A4 scenarios")
    for row in scenarios:
        exact_keys(row, {"scenario_id", "role_ids", "frame_budget_ns"}, "A4 scenario budget")
        if not isinstance(row["role_ids"], list) or not row["role_ids"] or any(
            role not in ROLE_IDS for role in row["role_ids"]
        ):
            raise V2AcceptanceError(f"A4 scenario {row['scenario_id']} has invalid role binding")
        _positive_int(row["frame_budget_ns"], f"A4 scenario {row['scenario_id']} frame budget")
    return policy, binding["authority"]["sha256"], nested_digests


def validate_sample(
    sample: Any, *, role_id: str, state: str, token: str, order: int, seed: int,
) -> dict[str, Any]:
    sample = exact_keys(sample, {
        "trial_id", "order", "seed", "first_visible_ns", "first_interaction_ns",
        "steady_cpu_frame_ns", "steady_gpu_frame_ns", "xrun_count",
        "audio_thread_work_events", "blank", "fallback_state",
        "trace_categories", "signatures_present",
    }, f"raw sample {token}")
    if sample["trial_id"] != token or sample["order"] != order or sample["seed"] != seed:
        raise V2AcceptanceError(f"raw sample {token} has the wrong deterministic order or seed")
    _positive_int(sample["first_visible_ns"], f"raw sample {token} first_visible_ns")
    if role_id == "headless-reference":
        if sample["first_interaction_ns"] is not None:
            raise V2AcceptanceError("headless-reference cannot fabricate interaction timing")
    else:
        _positive_int(sample["first_interaction_ns"], f"raw sample {token} first_interaction_ns")
    _positive_int(sample["steady_cpu_frame_ns"], f"raw sample {token} steady_cpu_frame_ns")
    _positive_int(sample["steady_gpu_frame_ns"], f"raw sample {token} steady_gpu_frame_ns")
    if sample["xrun_count"] != 0 or sample["audio_thread_work_events"] != 0:
        raise V2AcceptanceError(f"raw sample {token} reports an xrun or audio-thread work")
    if sample["blank"] is not False or sample["fallback_state"] not in {"prepared", "fallback"}:
        raise V2AcceptanceError(f"raw sample {token} does not prove a nonblank prepared/fallback frame")
    expected_categories = list(TRACE_CATEGORIES) if state == "candidate-active-session" else []
    if sample["trace_categories"] != expected_categories:
        raise V2AcceptanceError(f"raw sample {token} has missing or extra trace categories")
    if not isinstance(sample["signatures_present"], list) or not sample["signatures_present"]:
        raise V2AcceptanceError(f"raw sample {token} lacks bound signatures")
    return sample


def validate_raw_campaign(
    raw: Any, *, role_id: str, policy_sha256: str,
) -> dict[str, dict[str, list[dict[str, Any]]]]:
    raw = exact_keys(raw, {
        "schema", "version", "role_id", "manifest_seed", "trial_order",
        "timer_noise_samples_ns", "states",
    }, f"{role_id} raw samples")
    if raw["schema"] != "pulp.gpu-first-visible-a3-role-samples.v2" or raw["version"] != 2 or raw["role_id"] != role_id:
        raise V2AcceptanceError(f"{role_id} raw samples have the wrong identity")
    seed = derived_seed(policy_sha256, role_id)
    order = derived_trial_order(policy_sha256, role_id)
    if raw["manifest_seed"] != seed or raw["trial_order"] != order:
        raise V2AcceptanceError(f"{role_id} raw samples have the wrong pairing order or seed")
    timer = raw["timer_noise_samples_ns"]
    if not isinstance(timer, list) or len(timer) != 10000 or any(
        not isinstance(value, int) or isinstance(value, bool) or value < 0 for value in timer
    ):
        raise V2AcceptanceError(f"{role_id} timer-noise calibration must contain exactly 10000 integer pairs")
    states = raw["states"]
    if not isinstance(states, list) or [row.get("state") for row in states if isinstance(row, dict)] != list(STATES):
        raise V2AcceptanceError(f"{role_id} raw samples must contain the exact four ordered states")
    by_token = {token: index for index, token in enumerate(order)}
    result: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for row in states:
        exact_keys(row, {"state", "warmups", "warm", "cold"}, f"{role_id} state")
        state = row["state"]
        groups = {"warmup": row["warmups"], "warm": row["warm"], "cold": row["cold"]}
        for kind, count in (("warmup", 5), ("warm", 30), ("cold", 20)):
            samples = groups[kind]
            if not isinstance(samples, list) or len(samples) != count:
                raise V2AcceptanceError(f"{role_id} {state} must contain exactly {count} {kind} samples")
            for index, sample in enumerate(samples):
                token = f"{state}:{kind}:{index}"
                validate_sample(
                    sample, role_id=role_id, state=state, token=token,
                    order=by_token[token], seed=seed,
                )
        result[state] = groups
    return result


def campaign_budget_verdict(
    role_id: str, samples: dict[str, dict[str, list[dict[str, Any]]]],
    policy_role: dict[str, Any],
) -> str:
    shipping = samples["candidate-tracing-off"]
    measured = shipping["warm"] + shipping["cold"]
    misses = [
        percentile([row["first_visible_ns"] for row in measured], 95)
        > policy_role["first_visible_p95_ns"],
        percentile([row["steady_cpu_frame_ns"] for row in shipping["warm"]], 95)
        > policy_role["steady_cpu_frame_p95_ns"],
        percentile([row["steady_gpu_frame_ns"] for row in shipping["warm"]], 95)
        > policy_role["steady_gpu_frame_p95_ns"],
    ]
    if role_id != "headless-reference":
        misses.append(
            percentile([row["first_interaction_ns"] for row in measured], 95)
            > policy_role["first_interaction"]["p95_ns"]
        )
    return "fail" if any(misses) else "pass"


def collect_artifact_sha256s(receipt: dict[str, Any]) -> list[str]:
    digests: set[str] = set()

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            if set(value) == ARTIFACT_KEYS and SHA256.fullmatch(str(value.get("sha256", ""))):
                digests.add(value["sha256"])
            else:
                for nested in value.values():
                    visit(nested)
        elif isinstance(value, list):
            for nested in value:
                visit(nested)

    for key in ("product_policy", "campaigns", "observations"):
        visit(receipt[key])
    return sorted(digests)


def validate_v2(receipt: dict[str, Any], evidence_root: Path) -> bool:
    schema = load_json(SCHEMA_PATH)
    problems = json_schema_lite.validate(receipt, expand_local_schema_refs(schema, schema))
    if problems:
        raise V2AcceptanceError(f"receipt schema violation: {problems[0]}")
    if receipt["protocol"] != canonical_protocol():
        raise V2AcceptanceError("receipt protocol does not derive the exact A3 v2 matrix")
    policy_state = receipt["product_policy"]
    if policy_state["status"] == "missing":
        if (
            receipt["status"] != "blocked-product-policy"
            or receipt["blockers"] != list(BLOCKERS)
            or policy_state["authority"] is not None
            or policy_state["validation"] is not None
            or policy_state["required_coverage"] != "missing"
            or receipt["implementation_head"] is not None
            or receipt["campaigns"]
            or receipt["publication"] is not None
            or receipt["disposition"] is not None
        ):
            raise V2AcceptanceError("missing product policy must remain blocked-product-policy and nonterminal")
        return False
    if policy_state["required_coverage"] == "missing":
        if (
            receipt["status"] != "blocked-required-coverage"
            or receipt["blockers"] != ["required-coverage"]
            or policy_state["authority"] is None
            or policy_state["validation"] is None
            or receipt["implementation_head"] is not None
            or receipt["campaigns"]
            or receipt["publication"] is not None
            or receipt["disposition"] is not None
        ):
            raise V2AcceptanceError("missing constrained-adapter authority must remain blocked-required-coverage")
        resolve_artifact(
            policy_state["authority"], evidence_root, "product_policy.authority",
            loader=lambda path: path.read_bytes(),
        )
        resolve_artifact(
            policy_state["validation"], evidence_root, "product_policy.validation",
            loader=lambda path: path.read_bytes(),
        )
        return False
    if receipt["status"] != "complete" or receipt["blockers"]:
        raise V2AcceptanceError("bound policy and required coverage require complete status with no blockers")
    if not isinstance(receipt["implementation_head"], str) or not GIT_SHA.fullmatch(receipt["implementation_head"]):
        raise V2AcceptanceError("complete receipt requires an exact implementation head")
    policy, policy_sha256, policy_nested_digests = validate_product_policy(receipt, evidence_root)
    policy_roles = {row["role_id"]: row for row in policy["roles"]}
    campaigns = receipt["campaigns"]
    if [row.get("role_id") for row in campaigns if isinstance(row, dict)] != list(ROLE_IDS):
        raise V2AcceptanceError("complete receipt requires the exact seven ordered role campaigns")
    verdicts: dict[str, str] = {}
    for campaign in campaigns:
        role_id = campaign["role_id"]
        spec = ROLE_BY_ID[role_id]
        identity = campaign["identity"]
        if (
            identity["pulp_revision"] != receipt["implementation_head"]
            or identity["machine_id"] != policy["reference_host"]["machine_id"]
            or identity["host_kind"] != spec[1]
            or identity["application_kind"] != spec[2]
            or identity["plugin_format"] != spec[3]
        ):
            raise V2AcceptanceError(f"{role_id} host/app/format identity is not derived from its role")
        if role_id in FORGE_ROLES and identity["forge_revision"] != policy["first_applicable_forge_revision"]:
            raise V2AcceptanceError(f"{role_id} does not bind the authority Forge revision")
        if role_id not in FORGE_ROLES and identity["forge_revision"] is not None:
            raise V2AcceptanceError(f"{role_id} carries an unrelated Forge revision")
        if role_id == "constrained-adapter" and identity["adapter"] != policy["required_coverage"]["adapter"]:
            raise V2AcceptanceError("constrained-adapter does not use the authority-selected adapter")
        raw = resolve_artifact(campaign["raw_samples"], evidence_root, f"{role_id}.raw_samples")
        samples = validate_raw_campaign(raw, role_id=role_id, policy_sha256=policy_sha256)
        resolve_artifact(campaign["trace"], evidence_root, f"{role_id}.trace", loader=lambda path: path.read_bytes())
        analysis = resolve_artifact(campaign["trace_analysis"], evidence_root, f"{role_id}.trace_analysis")
        exact_keys(analysis, {"schema", "version", "role_id", "categories", "trace_complete", "dropped_events", "flush_complete"}, f"{role_id} trace analysis")
        if (
            analysis["schema"] != "pulp.gpu-first-visible-a3-trace-analysis.v2"
            or analysis["version"] != 2
            or analysis["role_id"] != role_id
            or analysis["categories"] != list(TRACE_CATEGORIES)
            or analysis["trace_complete"] is not True
            or analysis["dropped_events"] != 0
            or analysis["flush_complete"] is not True
        ):
            raise V2AcceptanceError(f"{role_id} trace is incomplete")
        verdict = campaign_budget_verdict(role_id, samples, policy_roles[role_id])
        if campaign["status"] != verdict:
            raise V2AcceptanceError(f"{role_id} disposition is not derived from raw thresholds")
        causal = campaign["causal_attribution"]
        if causal["instrumentation_complete"]:
            if causal["missing_events"] or causal["transferred_vellum_routes"]:
                raise V2AcceptanceError(f"{role_id} complete attribution cannot list missing instrumentation")
        elif not causal["missing_events"] or not causal["transferred_vellum_routes"]:
            raise V2AcceptanceError(f"{role_id} incomplete attribution must name missing events and transferred routes")
        verdicts[role_id] = verdict
    failures = [row for row in campaigns if verdicts[row["role_id"]] == "fail"]
    if not failures:
        derived_disposition = "no-change"
    elif any(row["causal_attribution"]["render_pipeline_material"] and row["causal_attribution"]["instrumentation_complete"] for row in failures):
        derived_disposition = "queue-B4"
    elif any(not row["causal_attribution"]["instrumentation_complete"] for row in failures):
        derived_disposition = "queue-B4-investigation"
    else:
        derived_disposition = "no-change"
    if receipt["disposition"] != derived_disposition:
        raise V2AcceptanceError("A3 disposition is not derived from thresholds and causal evidence")
    publication = receipt["publication"]
    if publication is None or publication["head"] != receipt["implementation_head"]:
        raise V2AcceptanceError("complete receipt is not bound to its exact protected-main head")
    expected_artifacts = sorted(
        set(collect_artifact_sha256s(receipt)) | policy_nested_digests
    )
    if publication["artifact_sha256s"] != expected_artifacts:
        raise V2AcceptanceError("protected-main publication does not enumerate every bound artifact digest")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify = subparsers.add_parser("verify")
    verify.add_argument("receipt", type=Path)
    verify.add_argument("--evidence-root", type=Path)
    args = parser.parse_args(argv)
    evidence_root = (args.evidence_root or args.receipt.parent).resolve()
    try:
        receipt = load_json(args.receipt.resolve())
        terminal = validate_v2(receipt, evidence_root)
    except V2AcceptanceError as error:
        print(f"A3 v2 acceptance: FAIL: {error}", file=sys.stderr)
        return 1
    if terminal:
        print("A3 v2 acceptance: PASS terminal=true")
        return 0
    blockers = receipt.get("blockers", [])
    print(
        "A3 v2 acceptance: NONTERMINAL terminal=false blockers="
        + ",".join(blockers)
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
