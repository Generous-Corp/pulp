#!/usr/bin/env python3
"""Closed semantic validator for GPU first-visible A3 acceptance v2."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable

import json_schema_lite
import gpu_first_visible_a3_trace_producer_overhead as trace_producer_overhead


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
PLANNING_POLICY_REPOSITORY = "danielraffel/pulp-planning"
PLANNING_POLICY_DIRECTORY = "research/evidence/gpu-ux/a3-budget"
A0_GPU_BASELINE = "docs/analysis/gpu-ux-baseline.md"
SUPPORT_MATRIX = "docs/status/support-matrix.yaml"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
POLICY_BUDGET_ID = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")
ARTIFACT_KEYS = {"path", "sha256"}
CANONICAL_RECEIPT = "docs/validation/gpu-first-visible-a3-acceptance.json"


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


def resolve_artifact_path(ref: Any, evidence_root: Path, label: str) -> Path:
    resolve_artifact(ref, evidence_root, label, loader=lambda path: path.read_bytes())
    path = (evidence_root / Path(ref["path"])).resolve()
    if not path.is_file() or path.is_symlink():
        raise V2AcceptanceError(f"{label} must be a regular non-symlink artifact")
    return path


def _command_json(command: list[str]) -> Any:
    completed = subprocess.run(
        command, stdin=subprocess.DEVNULL, capture_output=True, text=True,
        timeout=30, check=False,
    )
    if completed.returncode != 0:
        raise V2AcceptanceError(
            f"live proof command failed ({completed.returncode}): {completed.stderr.strip()}"
        )
    return json.loads(completed.stdout)


def _fetch_all_pages(ghapp: str, endpoint: str, *, object_key: str | None = None) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for page in range(1, 21):
        payload = _command_json([
            ghapp, "api", "--method", "GET", endpoint,
            "-f", "per_page=100", "-f", f"page={page}",
        ])
        batch = payload.get(object_key) if object_key and isinstance(payload, dict) else payload
        if not isinstance(batch, list):
            raise V2AcceptanceError(f"paginated endpoint returned the wrong shape: {endpoint}")
        rows.extend(row for row in batch if isinstance(row, dict))
        if len(batch) < 100:
            if object_key and isinstance(payload, dict):
                total = payload.get("total_count")
                if isinstance(total, int) and total != len(rows):
                    raise V2AcceptanceError(f"paginated endpoint was incomplete: {endpoint}")
            return rows
    raise V2AcceptanceError(f"paginated endpoint exceeded the 20-page safety bound: {endpoint}")


def _classic_protection(ghapp: str) -> dict[str, Any]:
    completed = subprocess.run(
        [ghapp, "api", "repos/Generous-Corp/pulp/branches/main/protection/required_status_checks"],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30, check=False,
    )
    if completed.returncode != 0:
        if "404" in completed.stderr:
            return {"contexts": [], "checks": []}
        raise V2AcceptanceError(f"classic protection query failed: {completed.stderr.strip()}")
    payload = json.loads(completed.stdout)
    return payload if isinstance(payload, dict) else {"contexts": [], "checks": []}


def required_check_identities(classic: dict[str, Any], rules: list[dict[str, Any]]) -> set[tuple[str, int | None]]:
    required: set[tuple[str, int | None]] = set()
    for context in classic.get("contexts", []):
        if isinstance(context, str) and context:
            required.add((context, None))
    for check in classic.get("checks", []):
        if isinstance(check, dict) and isinstance(check.get("context"), str):
            app_id = check.get("app_id")
            required.add((check["context"], app_id if isinstance(app_id, int) else None))
    for rule in rules:
        if rule.get("type") != "required_status_checks":
            continue
        for check in rule.get("parameters", {}).get("required_status_checks", []):
            if isinstance(check, dict) and isinstance(check.get("context"), str):
                app_id = check.get("integration_id")
                required.add((check["context"], app_id if isinstance(app_id, int) else None))
    return required


def required_check_result_errors(required: set[tuple[str, int | None]], runs: list[dict[str, Any]], statuses: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    for context, app_id in sorted(required, key=lambda item: (item[0], item[1] or -1)):
        candidates: list[tuple[str, int, bool]] = []
        for run in runs:
            observed_app = run.get("app", {}).get("id") if isinstance(run.get("app"), dict) else None
            if run.get("name") == context and (app_id is None or observed_app == app_id):
                candidates.append((str(run.get("completed_at") or run.get("started_at") or ""), int(run.get("id", 0)), run.get("status") == "completed" and run.get("conclusion") == "success"))
        if app_id is None:
            for status in statuses:
                if status.get("context") == context:
                    candidates.append((str(status.get("updated_at") or status.get("created_at") or ""), int(status.get("id", 0)), status.get("state") == "success"))
        if not candidates:
            errors.append(f"required check is missing: {context} app={app_id}")
            continue
        latest_time = max(item[0] for item in candidates)
        latest = [item for item in candidates if item[0] == latest_time]
        if len(latest) != 1:
            errors.append(f"required check has ambiguous latest results: {context}")
        elif latest[0][2] is not True:
            errors.append(f"latest required check is not successful: {context}")
    return errors


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


def git_blob_digest(data: bytes) -> str:
    """Return the Git object ID for regular-file bytes, not a security digest."""
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def _github_time(value: Any, label: str) -> dt.datetime:
    if not isinstance(value, str):
        raise V2AcceptanceError(f"{label} is missing")
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise V2AcceptanceError(f"{label} is invalid") from error
    if parsed.tzinfo is None:
        raise V2AcceptanceError(f"{label} must include a timezone")
    return parsed


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
        "schema", "version", "budget_id", "clock",
        "editor_open_origin", "first_nonblank_endpoint", "statistic",
        "first_applicable_pulp_revision", "first_applicable_forge_revision",
        "reference_host", "roles", "required_coverage", "a4_scenario_budgets",
        "canary",
    }, "product policy")
    if (
        policy["schema"] != "pulp.gpu-first-visible-budget-authority.v1"
        or policy["version"] != 1
        or not POLICY_BUDGET_ID.fullmatch(str(policy["budget_id"]))
        or policy["clock"] != "mach_continuous_time"
        or policy["editor_open_origin"] != "editor-open-requested"
        or policy["first_nonblank_endpoint"] != "first-nonblank-presented-frame"
        or policy["statistic"] != "p95"
    ):
        raise V2AcceptanceError("product policy has the wrong closed measurement semantics")
    exact_keys(validation, {
        "schema", "version", "status", "checked_at", "fresh_live_state",
        "protected_commit", "repository", "publication_commit", "path", "blob",
        "github_user_id", "approval_mode", "pr_url", "approved_head",
    }, "product policy validation")
    if (
        validation["schema"] != "pulp.gpu-first-visible-product-policy-validation.v1"
        or validation["version"] != 1
        or validation["status"] != "pass"
        or validation["fresh_live_state"] is not True
        or validation["protected_commit"] is not True
        or validation["github_user_id"] != 25807
        or validation["repository"] != PLANNING_POLICY_REPOSITORY
        or not GIT_SHA.fullmatch(str(validation["publication_commit"]))
        or not GIT_SHA.fullmatch(str(validation["approved_head"]))
        or not GIT_SHA.fullmatch(str(validation["blob"]))
        or validation["approval_mode"] not in {"author", "approval"}
        or not isinstance(validation["path"], str)
        or not validation["path"]
        or not isinstance(validation["pr_url"], str)
        or not validation["pr_url"].startswith("https://github.com/")
    ):
        raise V2AcceptanceError("product policy validation does not prove fresh protected authority")
    if (
        validation["path"]
        != f"{PLANNING_POLICY_DIRECTORY}/{policy['budget_id']}.product-policy.json"
    ):
        raise V2AcceptanceError("planning product policy path does not match its budget authority")
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
    policy_pulp_revision = policy["first_applicable_pulp_revision"]
    support_matrix_path = validate_generated_evidence(
        coverage["support_matrix"], evidence_root, "support-matrix",
        policy_pulp_revision, nested_digests,
    )
    if support_matrix_path != SUPPORT_MATRIX:
        raise V2AcceptanceError("required coverage is not bound to the protected support matrix")
    a1_paths: set[str] = set()
    for index, ref in enumerate(coverage["a1_evidence"]):
        a1_paths.add(validate_generated_evidence(
            ref, evidence_root, "a1-evidence", policy_pulp_revision,
            nested_digests,
        ))
        nested_digests.add(ref["sha256"])
    if A0_GPU_BASELINE not in a1_paths:
        raise V2AcceptanceError("required coverage lacks the A0-named protected GPU baseline")
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
    if canary["editor_open_origin"] != policy["editor_open_origin"]:
        raise V2AcceptanceError("product policy canary editor-open origin differs from measurement authority")
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


def validate_generated_evidence(
    wrapper_ref: dict[str, Any], evidence_root: Path, kind: str,
    implementation_head: str, nested_digests: set[str],
) -> str:
    wrapper = resolve_artifact(wrapper_ref, evidence_root, f"required coverage {kind}")
    exact_keys(wrapper, {
        "schema", "version", "kind", "implementation_head", "producer",
        "producer_provenance", "artifact",
    }, f"required coverage {kind} wrapper")
    if (
        wrapper["schema"] != "pulp.gpu-first-visible-generated-evidence.v1"
        or wrapper["version"] != 1 or wrapper["kind"] != kind
        or wrapper["implementation_head"] != implementation_head
    ):
        raise V2AcceptanceError(f"required coverage {kind} has the wrong identity")
    producer = resolve_artifact_path(wrapper["producer"], evidence_root, f"{kind}.producer")
    artifact = resolve_artifact_path(wrapper["artifact"], evidence_root, f"{kind}.artifact")
    provenance = resolve_artifact(wrapper["producer_provenance"], evidence_root, f"{kind}.producer_provenance")
    validate_existing_build_proof(
        provenance, wrapper["producer"], evidence_root, implementation_head,
        f"required coverage {kind} producer",
    )
    if not os.access(producer, os.X_OK):
        raise V2AcceptanceError(f"required coverage {kind} producer is not executable")
    completed = subprocess.run(
        [str(producer), "verify-a3-evidence", "--kind", kind, "--artifact", str(artifact), "--json"],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60, check=False,
    )
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise V2AcceptanceError(f"required coverage {kind} producer emitted invalid JSON") from error
    if (
        completed.returncode != 0
        or result != {
            "schema": "pulp.gpu-first-visible-evidence-verification.v1",
            "kind": kind, "artifact_sha256": wrapper["artifact"]["sha256"],
            "implementation_head": implementation_head, "valid": True,
        }
    ):
        raise V2AcceptanceError(f"required coverage {kind} was not reproduced by its pinned producer")
    nested_digests.update(
        wrapper[field]["sha256"] for field in ("producer", "producer_provenance", "artifact")
    )
    nested_digests.update(
        provenance[field]["sha256"]
        for field in ("build_verifier_receipt", "source_build_receipt")
    )
    return wrapper["artifact"]["path"]


def validate_existing_build_proof(
    provenance: Any, producer_ref: dict[str, Any], evidence_root: Path,
    implementation_head: str, label: str,
) -> None:
    exact_keys(provenance, {
        "schema", "version", "pulp_revision", "producer_sha256",
        "build_verifier_receipt", "source_build_receipt",
    }, f"{label} provenance")
    if (
        provenance["schema"] != "pulp.gpu-first-visible-evidence-producer.v1"
        or provenance["version"] != 1
        or provenance["pulp_revision"] != implementation_head
        or provenance["producer_sha256"] != producer_ref["sha256"]
    ):
        raise V2AcceptanceError(f"{label} provenance is invalid")
    embedded = resolve_artifact(
        provenance["build_verifier_receipt"], evidence_root,
        f"{label}.build_verifier_receipt",
    )
    source_build = resolve_artifact(
        provenance["source_build_receipt"], evidence_root,
        f"{label}.source_build_receipt",
    )
    exact_keys(embedded, {
        "schema", "version", "attempt_nonce", "control", "outcome", "reason",
        "verification_method", "product_identity", "product_sha256",
        "observed_product_sha256", "marker_sha256",
    }, f"{label} embedded build-verifier receipt")
    exact_keys(source_build, {
        "schema", "version", "attempt_nonce", "role", "outcome", "reason",
        "identity", "source_revisions", "build_command", "builder_id",
        "build_started_utc", "build_finished_utc", "driver_sha256", "product_path",
        "product_sha256", "bundle_path", "bundle_tree_sha256",
    }, f"{label} exact-source rebuild receipt")
    product_identity = embedded["product_identity"]
    exact_keys(product_identity, {
        "pulp_revision", "forge_revision", "build_id", "product_id",
        "product_name", "plugin_format",
    }, f"{label} product identity")
    if (
        embedded.get("schema") != "pulp.gpu-first-visible-build-verification-receipt.v1"
        or embedded.get("version") != 1
        or embedded.get("outcome") != "pass"
        or embedded.get("control") != "real"
        or embedded.get("verification_method") != "embedded-canonical-build-identity"
        or embedded.get("product_sha256") != producer_ref["sha256"]
        or embedded.get("observed_product_sha256") != producer_ref["sha256"]
        or embedded.get("reason") is not None
        or not SHA256.fullmatch(str(embedded.get("marker_sha256", "")))
        or product_identity["pulp_revision"] != implementation_head
    ):
        raise V2AcceptanceError(f"{label} lacks a passing existing embedded build-verifier receipt")
    revisions = source_build.get("source_revisions")
    if (
        source_build.get("schema") != "pulp.gpu-first-visible-source-build-receipt.v1"
        or source_build.get("version") != 1
        or source_build.get("outcome") != "pass"
        or source_build.get("reason") is not None
        or not isinstance(revisions, dict)
        or revisions.get("pulp") != implementation_head
        or source_build.get("product_sha256") != producer_ref["sha256"]
        or source_build.get("attempt_nonce") != embedded.get("attempt_nonce")
        or source_build.get("identity") != product_identity
        or not isinstance(source_build.get("build_command"), list)
        or not source_build["build_command"]
        or not isinstance(source_build.get("builder_id"), str)
        or not source_build["builder_id"]
        or not SHA256.fullmatch(str(source_build.get("driver_sha256", "")))
    ):
        raise V2AcceptanceError(f"{label} lacks a passing existing exact-source rebuild receipt")


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
    policy_interaction: dict[str, Any],
) -> dict[str, dict[str, list[dict[str, Any]]]]:
    raw = exact_keys(raw, {
        "schema", "version", "role_id", "manifest_seed", "trial_order",
        "interaction_authority", "timer_noise_samples_ns", "states",
    }, f"{role_id} raw samples")
    if raw["schema"] != "pulp.gpu-first-visible-a3-role-samples.v2" or raw["version"] != 2 or raw["role_id"] != role_id:
        raise V2AcceptanceError(f"{role_id} raw samples have the wrong identity")
    seed = derived_seed(policy_sha256, role_id)
    order = derived_trial_order(policy_sha256, role_id)
    if raw["manifest_seed"] != seed or raw["trial_order"] != order:
        raise V2AcceptanceError(f"{role_id} raw samples have the wrong pairing order or seed")
    expected_interaction = None
    if role_id in VISIBLE_ROLES:
        expected_interaction = {
            "origin": policy_interaction["origin"],
            "stimulus": policy_interaction["stimulus"],
            "expected_state": policy_interaction["expected_state_change"],
            "measurement_endpoint": policy_interaction["endpoint"],
        }
    if raw["interaction_authority"] != expected_interaction:
        raise V2AcceptanceError(f"{role_id} raw samples do not bind exact interaction authority")
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

    for key in (
        "product_policy", "campaigns", "blank_negative",
        "audio_thread_exclusion", "trace_producer_overhead", "observations",
    ):
        visit(receipt[key])
    return sorted(digests)


def _nested_artifact_digests(value: Any) -> set[str]:
    result: set[str] = set()
    if isinstance(value, dict):
        if set(value) == ARTIFACT_KEYS and SHA256.fullmatch(str(value.get("sha256", ""))):
            result.add(value["sha256"])
        else:
            for nested in value.values():
                result.update(_nested_artifact_digests(nested))
    elif isinstance(value, list):
        for nested in value:
            result.update(_nested_artifact_digests(nested))
    return result


def validate_control_receipts(receipt: dict[str, Any], evidence_root: Path) -> set[str]:
    nested_digests: set[str] = set()
    trace_digests = sorted(campaign["trace"]["sha256"] for campaign in receipt["campaigns"])
    blank = receipt["blank_negative"]
    if blank["status"] != "pass" or blank["receipt"] is None:
        raise V2AcceptanceError("terminal receipt requires a passing blank-frame negative")
    blank_payload = resolve_artifact(blank["receipt"], evidence_root, "blank_negative.receipt")
    exact_keys(blank_payload, {
        "schema", "version", "implementation_head", "campaign_trace_sha256s",
        "injected_blank_sha256", "diagnostic_code", "detected",
    }, "blank-frame negative receipt")
    if (
        blank_payload["schema"] != "pulp.gpu-first-visible-blank-negative.v2"
        or blank_payload["version"] != 2
        or blank_payload["implementation_head"] != receipt["implementation_head"]
        or blank_payload["campaign_trace_sha256s"] != trace_digests
        or not SHA256.fullmatch(str(blank_payload["injected_blank_sha256"]))
        or blank_payload["diagnostic_code"] != "gpu.startup.blank"
        or blank_payload["detected"] is not True
    ):
        raise V2AcceptanceError("blank-frame negative is not digest-bound to the terminal campaign")

    audio = receipt["audio_thread_exclusion"]
    if audio["status"] != "pass" or audio["receipt"] is None:
        raise V2AcceptanceError("terminal receipt requires passing external audio-thread exclusion")
    audio_payload = resolve_artifact(audio["receipt"], evidence_root, "audio_thread_exclusion.receipt")
    exact_keys(audio_payload, {
        "schema", "version", "implementation_head", "campaign_trace_sha256s",
        "executable", "scope", "provider_entry_points", "audio_thread_events",
        "non_audio_thread_events",
    }, "audio-thread exclusion receipt")
    executable = audio_payload["executable"]
    executable_path = resolve_artifact_path(executable, evidence_root, "audio_thread_exclusion.executable")
    nested_digests.add(executable["sha256"])
    expected_entries = [
        "begin_editor_open", "record_presented_frame", "record_timeout",
        "record_instance_lost", "record_dropped_events", "snapshot",
    ]
    if (
        audio_payload["schema"] != "pulp.gpu-first-visible-audio-thread-exclusion.v2"
        or audio_payload["version"] != 2
        or audio_payload["implementation_head"] != receipt["implementation_head"]
        or audio_payload["campaign_trace_sha256s"] != trace_digests
        or audio_payload["scope"] != "external-instrumented-harness"
        or audio_payload["provider_entry_points"] != expected_entries
        or audio_payload["audio_thread_events"] != 0
        or not isinstance(audio_payload["non_audio_thread_events"], int)
        or audio_payload["non_audio_thread_events"] <= 0
        or not os.access(executable_path, os.X_OK)
    ):
        raise V2AcceptanceError("external audio-thread exclusion proof is invalid")

    overhead = receipt["trace_producer_overhead"]
    if overhead["status"] != "pass" or overhead["reason"] is not None or overhead["receipt"] is None:
        raise V2AcceptanceError("terminal receipt requires passing four-state trace-producer overhead")
    overhead_payload = resolve_artifact(overhead["receipt"], evidence_root, "trace_producer_overhead.receipt")
    if overhead_payload.get("candidate_revision") != receipt["implementation_head"]:
        raise V2AcceptanceError("trace-producer overhead is not bound to implementation_head")
    try:
        trace_producer_overhead.validate_receipt(overhead_payload, evidence_root, require_pass=True)
    except trace_producer_overhead.OverheadError as error:
        raise V2AcceptanceError(f"four-state trace-producer overhead is invalid: {error}") from error
    nested_digests.update(_nested_artifact_digests(overhead_payload))
    return nested_digests


def replay_trace_analyzer(campaign: dict[str, Any], evidence_root: Path, implementation_head: str) -> set[str]:
    role_id = campaign["role_id"]
    trace_path = resolve_artifact_path(campaign["trace"], evidence_root, f"{role_id}.trace")
    sample_provenance = resolve_artifact(
        campaign["sample_provenance"], evidence_root, f"{role_id}.sample_provenance"
    )
    exact_keys(sample_provenance, {
        "schema", "version", "implementation_head", "role_id", "producer",
        "producer_provenance",
        "raw_samples_sha256", "trace_sha256", "identity_sha256",
    }, f"{role_id} sample provenance")
    identity_digest = hashlib.sha256(
        (json.dumps(campaign["identity"], sort_keys=True, separators=(",", ":")) + "\n").encode()
    ).hexdigest()
    if (
        sample_provenance["schema"] != "pulp.gpu-first-visible-a3-sample-provenance.v2"
        or sample_provenance["version"] != 2
        or sample_provenance["implementation_head"] != implementation_head
        or sample_provenance["role_id"] != role_id
        or sample_provenance["raw_samples_sha256"] != campaign["raw_samples"]["sha256"]
        or sample_provenance["trace_sha256"] != campaign["trace"]["sha256"]
        or sample_provenance["identity_sha256"] != identity_digest
    ):
        raise V2AcceptanceError(f"{role_id} samples lack exact producer and identity provenance")
    producer = resolve_artifact_path(sample_provenance["producer"], evidence_root, f"{role_id}.sample_producer")
    producer_provenance = resolve_artifact(
        sample_provenance["producer_provenance"], evidence_root,
        f"{role_id}.sample_producer_provenance",
    )
    validate_existing_build_proof(
        producer_provenance, sample_provenance["producer"], evidence_root,
        implementation_head, f"{role_id} sample producer",
    )
    if not os.access(producer, os.X_OK):
        raise V2AcceptanceError(f"{role_id} sample producer is not executable")
    raw_path = resolve_artifact_path(campaign["raw_samples"], evidence_root, f"{role_id}.raw_samples")
    completed = subprocess.run(
        [str(producer), "verify-a3-samples", "--raw", str(raw_path), "--trace", str(trace_path),
         "--identity-sha256", identity_digest, "--json"],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60, check=False,
    )
    try:
        producer_result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise V2AcceptanceError(f"{role_id} sample producer emitted invalid JSON") from error
    if completed.returncode != 0 or producer_result != {
        "schema": "pulp.gpu-first-visible-sample-verification.v2",
        "raw_samples_sha256": campaign["raw_samples"]["sha256"],
        "trace_sha256": campaign["trace"]["sha256"],
        "identity_sha256": identity_digest, "valid": True,
    }:
        raise V2AcceptanceError(f"{role_id} exact samples were not reproduced by their pinned producer")
    analyzer = resolve_artifact_path(campaign["trace_analyzer"], evidence_root, f"{role_id}.trace_analyzer")
    if not os.access(analyzer, os.X_OK):
        raise V2AcceptanceError(f"{role_id} trace analyzer is not executable")
    provenance = resolve_artifact(
        campaign["trace_analyzer_provenance"], evidence_root,
        f"{role_id}.trace_analyzer_provenance",
    )
    if (
        provenance.get("schema") != "pulp.gpu-first-visible-prepared-trace-analyzer.v1"
        or provenance.get("version") != 1
        or provenance.get("pulp_revision") != implementation_head
        or provenance.get("analyzer_sha256") != campaign["trace_analyzer"]["sha256"]
        or provenance.get("target_directory_fresh") is not True
        or provenance.get("cargo_home_mode") != "fresh-config-free-linked-locked-cache"
    ):
        raise V2AcceptanceError(f"{role_id} trace analyzer identity is invalid")
    completed = subprocess.run(
        [str(analyzer), "trace", "gpu-startup", "--trace", str(trace_path), "--json"],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60, check=False,
    )
    try:
        derived = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise V2AcceptanceError(f"{role_id} pinned analyzer did not emit JSON") from error
    binding = campaign["trace_binding"]
    scope = derived.get("category_scope")
    if (
        completed.returncode not in {0, 2}
        or derived.get("schema") != "pulp.trace-gpu-analysis.v1"
        or derived.get("question") != "gpu-startup"
        or derived.get("verdict") not in {"pass", "unverified"}
        or derived.get("capture_complete") is not True
        or derived.get("evidence_ids") != [binding["gpu_evidence_id"]]
        or scope != {
            "evidence_id": binding["gpu_evidence_id"],
            "process_upid": binding["process_upid"],
            "process_pid": binding["process_pid"],
        }
    ):
        raise V2AcceptanceError(f"{role_id} pinned trace replay does not prove the campaign cohort")
    analysis = resolve_artifact(campaign["trace_analysis"], evidence_root, f"{role_id}.trace_analysis")
    exact_keys(analysis, {
        "schema", "version", "role_id", "categories", "trace_complete",
        "dropped_events", "flush_complete", "trace_sha256", "campaign_id",
        "instance_id", "build_id", "gpu_evidence_id", "trace_evidence_id",
        "process_pid", "process_upid",
    }, f"{role_id} trace analysis")
    expected_binding = {
        key: binding[key] for key in (
            "campaign_id", "instance_id", "build_id", "gpu_evidence_id",
            "trace_evidence_id", "process_pid", "process_upid",
        )
    }
    if (
        analysis["schema"] != "pulp.gpu-first-visible-a3-trace-analysis.v2"
        or analysis["version"] != 2 or analysis["role_id"] != role_id
        or analysis["categories"] != list(TRACE_CATEGORIES)
        or analysis["trace_complete"] is not True or analysis["dropped_events"] != 0
        or analysis["flush_complete"] is not True
        or analysis["trace_sha256"] != campaign["trace"]["sha256"]
        or any(analysis[key] != value for key, value in expected_binding.items())
    ):
        raise V2AcceptanceError(f"{role_id} trace analysis is not bound to fresh replay and exact trace bytes")
    return {
        sample_provenance["producer"]["sha256"],
        sample_provenance["producer_provenance"]["sha256"],
        producer_provenance["build_verifier_receipt"]["sha256"],
        producer_provenance["source_build_receipt"]["sha256"],
    }


def product_policy_publication_errors(
    validation: dict[str, Any], policy_bytes: bytes, ghapp: str,
) -> list[str]:
    """Prove policy bytes, publication ancestry, and exact-head approval live."""
    errors: list[str] = []
    repository = validation["repository"]
    publication = validation["publication_commit"]
    policy_path = validation["path"]
    expected_blob = validation["blob"]
    approved_head = validation["approved_head"]
    if git_blob_digest(policy_bytes) != expected_blob:
        errors.append("bound product policy bytes do not match the protected Git blob")

    branch = _command_json([ghapp, "api", f"repos/{repository}/branches/main"])
    protected_head = branch.get("commit", {}).get("sha")
    if branch.get("protected") is not True or not GIT_SHA.fullmatch(str(protected_head)):
        errors.append("product policy repository main is not a live protected branch")
        return errors

    comparison = _command_json([
        ghapp, "api", f"repos/{repository}/compare/{publication}...{protected_head}",
    ])
    if (
        comparison.get("base_commit", {}).get("sha") != publication
        or comparison.get("merge_base_commit", {}).get("sha") != publication
        or comparison.get("status") not in {"ahead", "identical"}
        or comparison.get("behind_by") != 0
    ):
        errors.append("product policy publication is not an ancestor of protected main")

    for label, revision in (
        ("approved head", approved_head),
        ("publication", publication),
        ("protected main", protected_head),
    ):
        contents = _command_json([
            ghapp, "api", f"repos/{repository}/contents/{policy_path}?ref={revision}",
        ])
        if (
            contents.get("type") != "file"
            or contents.get("path") != policy_path
            or contents.get("sha") != expected_blob
        ):
            errors.append(f"product policy {label} does not contain the exact approved blob")

    match = re.fullmatch(
        r"https://github\.com/([^/]+/[^/]+)/pull/([0-9]+)", validation["pr_url"]
    )
    if match is None or match.group(1) != repository:
        errors.append("product policy approval URL does not identify its repository")
        return errors
    pr_number = int(match.group(2))
    pull = _command_json([ghapp, "api", f"repos/{repository}/pulls/{pr_number}"])
    created_at = pull.get("created_at")
    merged_at = pull.get("merged_at")
    if (
        pull.get("state") != "closed"
        or not isinstance(created_at, str)
        or not isinstance(merged_at, str)
        or pull.get("merge_commit_sha") != publication
        or pull.get("base", {}).get("ref") != "main"
    ):
        errors.append("product policy publication is not the recorded merged main PR")
        return errors
    created_time = _github_time(created_at, "product policy PR created_at")
    merged_time = _github_time(merged_at, "product policy PR merged_at")
    if created_time >= merged_time:
        errors.append("product policy PR merge does not postdate PR creation")
        return errors
    if pull.get("head", {}).get("sha") != approved_head:
        errors.append("product policy approval is not bound to the exact approved head")
        return errors
    if validation["approval_mode"] == "author":
        if (
            pull.get("user", {}).get("id") != 25807
            or pull.get("user", {}).get("type") != "User"
        ):
            errors.append("product policy author identity is not Daniel's immutable user ID")
        return errors

    reviews = _fetch_all_pages(ghapp, f"repos/{repository}/pulls/{pr_number}/reviews")
    effective: tuple[dt.datetime, int, dict[str, Any]] | None = None
    for review in reviews:
        if (
            review.get("user", {}).get("id") == 25807
            and review.get("user", {}).get("type") == "User"
            and review.get("commit_id") == approved_head
            and review.get("state") in {"APPROVED", "CHANGES_REQUESTED", "DISMISSED"}
        ):
            try:
                submitted = _github_time(
                    review.get("submitted_at"), "product policy review submitted_at"
                )
            except V2AcceptanceError:
                continue
            review_id = review.get("id")
            if created_time <= submitted <= merged_time and isinstance(review_id, int):
                candidate = (submitted, review_id, review)
                if effective is None or candidate[:2] > effective[:2]:
                    effective = candidate
    if effective is None or effective[2].get("state") != "APPROVED":
        errors.append("product policy lacks an effective pre-merge exact-head Daniel approval")
    return errors


def protected_required_coverage_errors(
    policy: dict[str, Any], evidence_root: Path, pulp_head: str, ghapp: str,
) -> list[str]:
    """Bind generated required-coverage inputs to their protected Pulp bytes."""
    coverage = policy["required_coverage"]
    refs = [coverage["support_matrix"], *coverage["a1_evidence"]]
    errors: list[str] = []
    policy_revision = policy["first_applicable_pulp_revision"]
    comparison = _command_json([
        ghapp, "api", f"repos/Generous-Corp/pulp/compare/{policy_revision}...{pulp_head}",
    ])
    if (
        comparison.get("base_commit", {}).get("sha") != policy_revision
        or comparison.get("merge_base_commit", {}).get("sha") != policy_revision
        or comparison.get("status") != "ahead"
        or not isinstance(comparison.get("ahead_by"), int)
        or comparison["ahead_by"] <= 0
        or comparison.get("behind_by") != 0
    ):
        errors.append("product policy first-applicable Pulp revision is not strict protected-main ancestry")
    for ref in refs:
        wrapper = resolve_artifact(ref, evidence_root, "required coverage live wrapper")
        artifact = resolve_artifact_path(
            wrapper["artifact"], evidence_root, "required coverage live artifact",
        )
        path = wrapper["artifact"]["path"]
        contents = _command_json([
            ghapp, "api", f"repos/Generous-Corp/pulp/contents/{path}?ref={pulp_head}",
        ])
        if (
            contents.get("type") != "file"
            or contents.get("path") != path
            or contents.get("sha") != git_blob_digest(artifact.read_bytes())
        ):
            errors.append(f"required coverage is not the exact protected Pulp blob: {path}")
    return errors


def live_protected_main_errors(
    receipt: dict[str, Any], receipt_path: Path, repository: Path, evidence_root: Path,
) -> list[str]:
    errors: list[str] = []
    ghapp = shutil.which("ghapp")
    if ghapp is None:
        return ["live protected-main proof requires ghapp"]
    try:
        expected_path = (repository / CANONICAL_RECEIPT).resolve()
        if receipt_path.resolve() != expected_path or expected_path.is_symlink():
            errors.append("terminal validation requires the exact canonical receipt path")
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repository, check=True, capture_output=True, text=True, timeout=10).stdout.strip()
        if subprocess.run(["git", "status", "--porcelain"], cwd=repository, check=True, capture_output=True, text=True, timeout=10).stdout:
            errors.append("terminal validation requires a clean checkout")
        branch = _command_json([ghapp, "api", "repos/Generous-Corp/pulp/branches/main"])
        live_head = branch.get("commit", {}).get("sha")
        if live_head != head or head != receipt["implementation_head"]:
            errors.append("receipt implementation_head is not exact live protected main")
        if branch.get("protected") is not True:
            errors.append("live main is not protected")
        contents = _command_json([ghapp, "api", f"repos/Generous-Corp/pulp/contents/{CANONICAL_RECEIPT}?ref={head}"])
        local_blob = subprocess.run(["git", "hash-object", CANONICAL_RECEIPT], cwd=repository, check=True, capture_output=True, text=True, timeout=10).stdout.strip()
        indexed_blob = subprocess.run(["git", "rev-parse", f"HEAD:{CANONICAL_RECEIPT}"], cwd=repository, check=True, capture_output=True, text=True, timeout=10).stdout.strip()
        if contents.get("type") != "file" or contents.get("path") != CANONICAL_RECEIPT or contents.get("sha") != local_blob or local_blob != indexed_blob:
            errors.append("live main does not contain the exact canonical receipt blob")
        policy_path = resolve_artifact_path(
            receipt["product_policy"]["authority"], evidence_root,
            "product_policy.authority",
        )
        validation = resolve_artifact(
            receipt["product_policy"]["validation"], evidence_root,
            "product_policy.validation",
        )
        policy_bytes = policy_path.read_bytes()
        errors.extend(product_policy_publication_errors(validation, policy_bytes, ghapp))
        errors.extend(protected_required_coverage_errors(
            json.loads(policy_bytes), evidence_root, head, ghapp,
        ))
        classic = _classic_protection(ghapp)
        rules = _fetch_all_pages(ghapp, "repos/Generous-Corp/pulp/rules/branches/main")
        required = required_check_identities(classic, rules)
        if not required:
            errors.append("protected main exposes no required status checks")
        runs = _fetch_all_pages(ghapp, f"repos/Generous-Corp/pulp/commits/{head}/check-runs", object_key="check_runs")
        statuses = _fetch_all_pages(ghapp, f"repos/Generous-Corp/pulp/commits/{head}/statuses")
        errors.extend(required_check_result_errors(required, runs, statuses))
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError, V2AcceptanceError) as error:
        errors.append(f"live protected-main proof failed: {error}")
    return errors


def validate_v2(
    receipt: dict[str, Any], evidence_root: Path, *, receipt_path: Path | None = None,
    repository: Path | None = None,
) -> bool:
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
            or receipt["blank_negative"] != {"status": "missing", "receipt": None}
            or receipt["audio_thread_exclusion"] != {"status": "missing", "receipt": None}
            or receipt["trace_producer_overhead"]["status"] not in {"missing", "unavailable"}
            or receipt["trace_producer_overhead"]["receipt"] is not None
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
            or receipt["blank_negative"] != {"status": "missing", "receipt": None}
            or receipt["audio_thread_exclusion"] != {"status": "missing", "receipt": None}
            or receipt["trace_producer_overhead"]["status"] not in {"missing", "unavailable"}
            or receipt["trace_producer_overhead"]["receipt"] is not None
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
    campaign_by_role = {row["role_id"]: row for row in campaigns}
    standalone_identity = campaign_by_role["pulp-standalone"]["identity"]
    constrained_identity = campaign_by_role["constrained-adapter"]["identity"]
    canary = policy["canary"]
    for role_id, identity in (
        ("pulp-standalone", standalone_identity),
        ("constrained-adapter", constrained_identity),
    ):
        if (
            identity["application_sha256"] != canary["binary_sha256"]
            or identity["content_sha256"] != canary["content_sha256"]
            or identity["signature_sha256"] != canary["signature_sha256"]
            or identity.get("editor_open_origin") != canary["editor_open_origin"]
            or identity.get("interaction_lifecycle") != canary["interaction_lifecycle"]
            or identity.get("steady_state_workload") != canary["steady_state_workload"]
        ):
            raise V2AcceptanceError(f"{role_id} is not the authority-bound Pulp standalone canary")
    coverage = policy["required_coverage"]
    if (
        not isinstance(standalone_identity.get("adapter_configuration"), str)
        or not standalone_identity["adapter_configuration"]
        or constrained_identity.get("adapter_predicate") != coverage["predicate"]
        or constrained_identity.get("adapter_configuration") != coverage["configuration"]
    ):
        raise V2AcceptanceError("standalone campaigns do not bind the authority adapter configurations")
    allowed_deltas = {"adapter", "adapter_predicate", "adapter_configuration"}
    standalone_product = {
        key: value for key, value in standalone_identity.items() if key not in allowed_deltas
    }
    constrained_product = {
        key: value for key, value in constrained_identity.items() if key not in allowed_deltas
    }
    if standalone_product != constrained_product:
        raise V2AcceptanceError("constrained-adapter changes more than adapter/configuration")
    verdicts: dict[str, str] = {}
    nested_artifact_digests: set[str] = set(policy_nested_digests)
    for campaign in campaigns:
        role_id = campaign["role_id"]
        spec = ROLE_BY_ID[role_id]
        identity = campaign["identity"]
        if (
            identity["pulp_revision"] != receipt["implementation_head"]
            or identity["machine_id"] != policy["reference_host"]["machine_id"]
            or identity["hardware"] != policy["reference_host"]["hardware"]
            or identity["os"] != policy["reference_host"]["os"]
            or identity["display_id"] != policy["reference_host"]["display"]
            or identity["refresh_hz"] != policy["reference_host"]["refresh_hz"]
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
        policy_interaction = policy_roles[role_id]["first_interaction"]
        interaction_fields = {
            "interaction_origin": "origin",
            "interaction_stimulus": "stimulus",
            "interaction_expected_state": "expected_state_change",
            "interaction_measurement_endpoint": "endpoint",
        }
        if role_id in VISIBLE_ROLES:
            if any(identity.get(field) != policy_interaction[policy_field]
                   for field, policy_field in interaction_fields.items()):
                raise V2AcceptanceError(f"{role_id} campaign does not bind exact interaction authority")
        elif any(field in identity for field in interaction_fields):
            raise V2AcceptanceError("headless-reference cannot carry visible interaction authority")
        raw = resolve_artifact(campaign["raw_samples"], evidence_root, f"{role_id}.raw_samples")
        samples = validate_raw_campaign(
            raw, role_id=role_id, policy_sha256=policy_sha256,
            policy_interaction=policy_interaction,
        )
        nested_artifact_digests.update(
            replay_trace_analyzer(campaign, evidence_root, receipt["implementation_head"])
        )
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
    nested_artifact_digests.update(validate_control_receipts(receipt, evidence_root))
    publication = receipt["publication"]
    if publication is None:
        raise V2AcceptanceError("complete receipt lacks a terminal publication request")
    expected_artifacts = sorted(
        set(collect_artifact_sha256s(receipt)) | nested_artifact_digests
    )
    if publication["artifact_sha256s"] != expected_artifacts:
        raise V2AcceptanceError("protected-main publication does not enumerate every bound artifact digest")
    if receipt_path is None or repository is None:
        raise V2AcceptanceError("terminal publication requires live canonical-path verification")
    live_errors = live_protected_main_errors(receipt, receipt_path, repository, evidence_root)
    if live_errors:
        raise V2AcceptanceError("; ".join(live_errors))
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
        repository = Path(__file__).resolve().parents[2]
        terminal = validate_v2(
            receipt, evidence_root, receipt_path=args.receipt.resolve(), repository=repository,
        )
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
