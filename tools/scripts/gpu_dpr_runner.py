#!/usr/bin/env python3
"""Run and ingest resumable A4 DPR trials without selecting render policy."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_dpr_experiment as experiment  # noqa: E402

RUN_SCHEMA = "pulp.gpu-dpr-run-state.v1"
REQUEST_SCHEMA = "pulp.gpu-dpr-cell-request.v1"
RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-receipt.v1"
RAW_SCHEMA = "pulp.gpu-dpr-raw-samples.v1"
B5_SCHEMA = "pulp.gpu-dpr-b5-gate.v1"
COMPLETE_OUTCOMES = {"pass", "fail"}
INCOMPLETE_OUTCOMES = {"skip", "inconclusive"}
ALL_OUTCOMES = COMPLETE_OUTCOMES | INCOMPLETE_OUTCOMES
ARTIFACT_KINDS = {"capture", "trace", "raw_samples", "input_receipt"}
TRACE_QUESTIONS = {"gpu-startup", "gpu-health", "gpu-probe"}
METRIC_UNITS = {
    "cpu_frame_time": "ms",
    "gpu_frame_time": "ms",
    "first_frame_time": "ms",
    "interaction_latency": "ms",
    "render_target_bytes": "bytes",
    "resident_bytes": "bytes",
    "upload_bytes": "bytes",
}


class EvidenceError(ValueError):
    """A receipt exists but cannot support a measured A4 cell."""


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, sort_keys=True, indent=2) + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False
    ) as handle:
        handle.write(payload)
        temporary = Path(handle.name)
    os.replace(temporary, path)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cell_key(scenario_id: str, mode: str, dpr: float) -> str:
    rendered_dpr = str(int(dpr)) if float(dpr).is_integer() else str(dpr)
    return f"{scenario_id}__{mode}__dpr-{rendered_dpr}"


def parse_cell_key(key: str) -> tuple[str, str, float]:
    scenario, mode, dpr = key.rsplit("__", 2)
    return scenario, mode, float(dpr.removeprefix("dpr-"))


def scenario_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["id"]: item for item in manifest["scenarios"]}


def source_digest(
    scenario: dict[str, Any], manifest_path: Path, forge_sha: str | None
) -> str:
    if scenario.get("source_sha256"):
        return scenario["source_sha256"]
    identity = scenario["source"]
    if scenario["kind"].startswith("external_forge"):
        if forge_sha is None:
            raise EvidenceError(f"{scenario['id']}: exact Forge SHA is required")
        identity += f"@{forge_sha}"
    return hashlib.sha256(identity.encode("utf-8")).hexdigest()


def initial_state(
    plan: dict[str, Any], manifest: dict[str, Any], manifest_path: Path
) -> dict[str, Any]:
    if plan.get("schema") != "pulp.gpu-dpr-experiment.v1":
        raise EvidenceError("plan is not a Pulp A4 DPR experiment document")
    if plan.get("status") != "planned" or plan.get("observations"):
        raise EvidenceError("runner initialization requires an empty planned result")
    expected_digest = experiment.canonical_sha256(manifest)
    if plan["matrix"]["manifest_sha256"] != expected_digest:
        raise EvidenceError("plan and corpus manifest digests differ")
    if plan["matrix"]["scenario_ids"] != [item["id"] for item in manifest["scenarios"]]:
        raise EvidenceError("plan scenario ordering differs from the corpus")

    cells: dict[str, Any] = {}
    scenarios = scenario_map(manifest)
    for scenario_id, mode, dpr in sorted(experiment.expected_matrix(manifest)):
        scenario = scenarios[scenario_id]
        key = cell_key(scenario_id, mode, dpr)
        cells[key] = {
            "scenario_id": scenario_id,
            "scenario_kind": scenario["kind"],
            "mode": mode,
            "requested_dpr": dpr,
            "status": "pending",
            "attempts": [],
            "observation": None,
            "dependencies": [],
        }
    return {
        "schema": RUN_SCHEMA,
        "version": 1,
        "experiment_id": plan["experiment_id"],
        "plan_sha256": experiment.canonical_sha256(plan),
        "manifest_sha256": expected_digest,
        "manifest_path": str(manifest_path.resolve()),
        "plan": plan,
        "cells": cells,
    }


def run_paths(run_dir: Path) -> tuple[Path, Path, Path]:
    return run_dir / "run-state.json", run_dir / "result.json", run_dir / "b5-gate.json"


def project_result(state: dict[str, Any]) -> dict[str, Any]:
    result = json.loads(json.dumps(state["plan"]))
    observations = [
        cell["observation"]
        for cell in state["cells"].values()
        if cell["observation"] is not None
    ]
    result["observations"] = sorted(
        observations,
        key=lambda item: (
            item["scenario_id"],
            experiment.MODES.index(item["mode"]),
            float(item["requested_dpr"]),
        ),
    )
    if any(cell["status"] != "complete" for cell in state["cells"].values()):
        attempted = any(cell["attempts"] for cell in state["cells"].values())
        result["status"] = "incomplete" if observations or attempted else "planned"
        result["disposition"] = None
        result["eligible_for_policy"] = False
    return result


def save_state(run_dir: Path, state: dict[str, Any]) -> None:
    state_path, result_path, _ = run_paths(run_dir)
    atomic_json(state_path, state)
    atomic_json(result_path, project_result(state))


def load_state(run_dir: Path) -> dict[str, Any]:
    state_path, _, _ = run_paths(run_dir)
    if not state_path.is_file():
        raise EvidenceError(f"run state is missing: {state_path}")
    state = load_json(state_path)
    if state.get("schema") != RUN_SCHEMA or state.get("version") != 1:
        raise EvidenceError("unsupported DPR run-state schema")
    return state


def cell_directory(run_dir: Path, key: str) -> Path:
    return run_dir / "cells" / key


def safe_artifact(cell_dir: Path, relative: str) -> Path:
    candidate = (cell_dir / relative).resolve()
    root = cell_dir.resolve()
    if candidate != root and root not in candidate.parents:
        raise EvidenceError(f"artifact escapes its cell directory: {relative}")
    if not candidate.is_file():
        raise EvidenceError(f"artifact is missing: {relative}")
    return candidate


def nearest_rank_p95(samples: list[float]) -> float:
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def metric_statistic(name: str, samples: Any, manifest: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(samples, list) or not samples:
        raise EvidenceError(f"raw metric {name} has no samples")
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) for value in samples):
        raise EvidenceError(f"raw metric {name} contains a non-number")
    values = [float(value) for value in samples]
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise EvidenceError(f"raw metric {name} contains an invalid value")
    trial = manifest["trial_contract"]
    minimum = (
        trial["fresh_process_first_frame_trials"]
        if name == "first_frame_time"
        else trial["measured_trials"]
    )
    if len(values) < minimum:
        raise EvidenceError(f"raw metric {name} has {len(values)} samples; needs {minimum}")
    median = statistics.median(values)
    return {
        "unit": METRIC_UNITS[name],
        "median": median,
        "p95": max(median, nearest_rank_p95(values)),
        "sample_count": len(values),
    }


def validate_logical_input(raw: dict[str, Any]) -> bool:
    trials = raw.get("logical_input_trials")
    if not isinstance(trials, list) or not trials:
        raise EvidenceError("logical-input oracle has no trials")
    passed = True
    for trial in trials:
        expected = trial.get("expected_logical")
        observed = trial.get("observed_logical")
        if not (
            isinstance(expected, list)
            and isinstance(observed, list)
            and len(expected) == 2
            and len(observed) == 2
            and all(isinstance(value, (int, float)) for value in [*expected, *observed])
        ):
            raise EvidenceError("logical-input trial is malformed")
        coordinate_match = all(
            math.isclose(float(left), float(right), abs_tol=1e-6)
            for left, right in zip(expected, observed)
        )
        target_match = trial.get("expected_target") == trial.get("observed_target")
        passed = passed and coordinate_match and target_match
    return passed


def validate_trace(raw: dict[str, Any], manifest: dict[str, Any]) -> list[str]:
    trace = raw.get("trace")
    if not isinstance(trace, dict) or trace.get("complete") is not True:
        raise EvidenceError("trace receipt is absent or incomplete")
    categories = set(trace.get("categories", []))
    missing = set(manifest["trial_contract"]["required_trace_categories"]) - categories
    if missing:
        raise EvidenceError(f"trace categories are missing: {sorted(missing)}")
    questions = trace.get("questions")
    if not isinstance(questions, list):
        raise EvidenceError("trace named-question receipts are missing")
    by_id = {item.get("id"): item for item in questions}
    if set(by_id) != TRACE_QUESTIONS:
        raise EvidenceError("trace named-question coverage must be gpu-startup/health/probe")
    for question in ("gpu-health", "gpu-probe"):
        if by_id[question].get("status") != "pass":
            raise EvidenceError(f"trace question {question} did not pass")
    if by_id["gpu-startup"].get("status") not in {"pass", "unverified"}:
        raise EvidenceError("trace question gpu-startup is unavailable or failed")
    evidence_ids = [item.get("evidence_id") for item in questions]
    if any(not isinstance(item, str) or not item for item in evidence_ids):
        raise EvidenceError("trace named-question evidence id is missing")
    if len(evidence_ids) != len(set(evidence_ids)):
        raise EvidenceError("trace evidence ids are not unique")
    return evidence_ids


def validate_identity(
    receipt: dict[str, Any], scenario: dict[str, Any], plan: dict[str, Any]
) -> None:
    machine = receipt.get("machine")
    adapter = receipt.get("adapter")
    if not isinstance(machine, dict) or not machine.get("id"):
        raise EvidenceError("machine receipt lacks an id")
    if not machine.get("os") or not machine.get("architecture"):
        raise EvidenceError("machine receipt lacks OS/architecture identity")
    if not isinstance(adapter, dict) or adapter.get("class") not in {
        "hardware", "software", "unknown"
    }:
        raise EvidenceError("adapter receipt lacks a valid class")
    if not all(adapter.get(field) for field in ("name", "backend", "driver")):
        raise EvidenceError("adapter receipt lacks name/backend/driver identity")
    build = receipt.get("build_identity")
    if not isinstance(build, dict) or build.get("pulp_sha") != plan["pulp_sha"]:
        raise EvidenceError("receipt is not bound to the planned Pulp SHA")

    requirements = set(scenario["required_oracles"])
    if "authentic_gpu" in requirements:
        if adapter["class"] != "hardware" or adapter.get("authentic_identity") is not True:
            raise EvidenceError("scenario requires an authentic hardware GPU adapter")
    if "authentic_webgl" in requirements:
        if adapter["class"] != "hardware" or adapter.get("api") != "webgl2":
            raise EvidenceError("web canary requires authentic WebGL2 hardware evidence")
    if "exact_binary_identity" in requirements:
        binary = build.get("binary")
        if not isinstance(binary, dict) or not binary.get("path") or not binary.get("sha256"):
            raise EvidenceError("Forge receipt lacks exact binary identity")
        if build.get("forge_sha") != plan["forge_sha"] or plan["forge_sha"] is None:
            raise EvidenceError("Forge receipt is not bound to the planned Forge SHA")
    if "exact_plugin_format" in requirements:
        if receipt.get("plugin_format") not in {"au", "vst3", "clap"}:
            raise EvidenceError("DAW receipt lacks an exact plugin format")
        if receipt.get("format_qualified") is not True or receipt.get("scan_cache_confirmed") is not True:
            raise EvidenceError("DAW receipt did not prove format-qualified scan/load")
    if "audio_thread_excluded" in requirements and receipt.get("audio_thread_excluded") is not True:
        raise EvidenceError("scenario did not prove audio-thread exclusion")


def receipt_observation(
    receipt_path: Path,
    state: dict[str, Any],
    manifest: dict[str, Any],
    manifest_path: Path,
    run_dir: Path,
) -> tuple[str, dict[str, Any] | None, list[str]]:
    receipt = load_json(receipt_path)
    if receipt.get("schema") != RECEIPT_SCHEMA or receipt.get("version") != 1:
        raise EvidenceError("unsupported DPR cell receipt schema")
    outcome = receipt.get("outcome")
    if outcome not in ALL_OUTCOMES:
        raise EvidenceError("receipt outcome must be pass/fail/skip/inconclusive")
    requested = receipt.get("requested_dpr")
    if isinstance(requested, bool) or not isinstance(requested, (int, float)):
        raise EvidenceError("receipt requested DPR is not numeric")
    key = cell_key(receipt.get("scenario_id", ""), receipt.get("mode", ""), float(requested))
    if key not in state["cells"]:
        raise EvidenceError(f"receipt names an unexpected matrix cell: {key}")
    cell = state["cells"][key]
    scenario = scenario_map(manifest)[cell["scenario_id"]]
    if receipt.get("scenario_kind") != scenario["kind"]:
        raise EvidenceError("receipt scenario kind differs from the corpus")

    reason = receipt.get("reason")
    dependencies = receipt.get("dependencies", [])
    if outcome in INCOMPLETE_OUTCOMES:
        if not isinstance(reason, str) or not reason:
            raise EvidenceError(f"{outcome} receipt requires a reason")
        if (
            not isinstance(dependencies, list)
            or not dependencies
            or any(not isinstance(item, str) or not item for item in dependencies)
        ):
            raise EvidenceError(f"{outcome} receipt requires explicit dependencies")
        return key, None, dependencies

    validate_identity(receipt, scenario, state["plan"])
    cell_dir = cell_directory(run_dir, key)
    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, list):
        raise EvidenceError("receipt artifact list is missing")
    by_kind: dict[str, dict[str, Any]] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise EvidenceError("receipt contains a malformed artifact")
        kind = artifact.get("kind")
        if kind not in ARTIFACT_KINDS or kind in by_kind:
            raise EvidenceError("artifact kinds must be unique capture/trace/raw_samples/input_receipt")
        path = safe_artifact(cell_dir, artifact.get("path", ""))
        digest = artifact.get("sha256")
        if sha256_file(path) != digest:
            raise EvidenceError(f"{kind} artifact digest does not match its bytes")
        by_kind[kind] = {
            "kind": kind,
            "path": f"cells/{key}/{artifact['path']}",
            "sha256": digest,
        }
    if set(by_kind) != ARTIFACT_KINDS:
        raise EvidenceError("cell requires capture/trace/raw_samples/input_receipt artifacts")

    raw_artifact = next(item for item in artifacts if item["kind"] == "raw_samples")
    raw_path = safe_artifact(cell_dir, raw_artifact["path"])
    raw = load_json(raw_path)
    if raw.get("schema") != RAW_SCHEMA or raw.get("version") != 1:
        raise EvidenceError("unsupported raw-sample schema")
    metrics_raw = raw.get("metrics")
    if not isinstance(metrics_raw, dict) or set(metrics_raw) != set(METRIC_UNITS):
        raise EvidenceError("raw samples do not cover the exact metric set")
    metrics = {
        name: metric_statistic(name, metrics_raw[name], manifest)
        for name in METRIC_UNITS
    }
    trace_ids = validate_trace(raw, manifest)

    logical_input_passed = validate_logical_input(raw)
    fidelity_raw = raw.get("fidelity")
    if not isinstance(fidelity_raw, dict):
        raise EvidenceError("fidelity oracle receipt is missing")
    fidelity = {
        "content_floor_passed": fidelity_raw.get("content_floor_passed") is True,
        "capture_similarity": fidelity_raw.get("capture_similarity"),
        "small_text_legible": fidelity_raw.get("small_text_legible") is True,
        "thin_strokes_preserved": fidelity_raw.get("thin_strokes_preserved") is True,
        "logical_input_correct": logical_input_passed,
    }
    if not isinstance(fidelity["capture_similarity"], (int, float)) or not 0 <= fidelity["capture_similarity"] <= 1:
        raise EvidenceError("capture similarity is outside 0..1")
    all_fidelity = all(
        fidelity[name]
        for name in (
            "content_floor_passed", "small_text_legible",
            "thin_strokes_preserved", "logical_input_correct"
        )
    )
    if outcome == "pass" and not all_fidelity:
        raise EvidenceError("pass receipt contains a failing fidelity oracle")
    if outcome == "fail" and all_fidelity:
        raise EvidenceError("fail receipt contains no failing fidelity oracle")

    observed_dpr = receipt.get("observed_dpr")
    if not isinstance(observed_dpr, (int, float)) or observed_dpr <= 0:
        raise EvidenceError("receipt lacks a positive observed DPR")
    requested_dpr = float(cell["requested_dpr"])
    configured_max: float | None = None
    adaptive: str | None = None
    if cell["mode"] == "exact":
        expected_dpr = requested_dpr
    elif cell["mode"] == "configured_max":
        configured_max = float(manifest["configured_max_dpr"])
        expected_dpr = min(requested_dpr, configured_max)
    else:
        adaptive = manifest["adaptive_profile"]["id"]
        expected_dpr = requested_dpr
    if not math.isclose(float(observed_dpr), expected_dpr, abs_tol=1e-9):
        raise EvidenceError("observed DPR differs from the requested experiment mode")
    logical = scenario["logical_size"]
    physical = receipt.get("physical_size")
    expected_physical = {
        "width": round(logical["width"] * expected_dpr),
        "height": round(logical["height"] * expected_dpr),
    }
    if physical != expected_physical:
        raise EvidenceError("physical size does not equal logical size x observed DPR")
    expected_content = source_digest(scenario, manifest_path, state["plan"]["forge_sha"])
    if receipt.get("content_digest") != expected_content:
        raise EvidenceError("receipt content digest differs from the frozen scenario")

    observation = {
        "scenario_id": cell["scenario_id"],
        "mode": cell["mode"],
        "requested_dpr": cell["requested_dpr"],
        "observed_dpr": observed_dpr,
        "configured_max_dpr": configured_max,
        "adaptive_profile_id": adaptive,
        "logical_size": logical,
        "physical_size": physical,
        "content_digest": expected_content,
        "machine_id": receipt["machine"]["id"],
        "adapter_class": receipt["adapter"]["class"],
        "metrics": metrics,
        "fidelity": fidelity,
        "trace_evidence_ids": trace_ids,
        "artifacts": [by_kind[kind] for kind in sorted(by_kind)],
    }
    return key, observation, []


def record_attempt(
    state: dict[str, Any], key: str, outcome: str, receipt_path: str | None,
    reason: str | None, dependencies: list[str], observation: dict[str, Any] | None
) -> None:
    cell = state["cells"][key]
    cell["attempts"].append({
        "number": len(cell["attempts"]) + 1,
        "outcome": outcome,
        "receipt": receipt_path,
        "reason": reason,
        "dependencies": dependencies,
    })
    cell["dependencies"] = dependencies
    cell["observation"] = observation
    cell["status"] = "complete" if outcome in COMPLETE_OUTCOMES else outcome


def ingest_receipt(run_dir: Path, receipt_path: Path) -> str:
    state = load_state(run_dir)
    manifest_path = Path(state["manifest_path"])
    manifest = load_json(manifest_path)
    receipt = load_json(receipt_path)
    outcome = receipt.get("outcome", "inconclusive")
    key, observation, dependencies = receipt_observation(
        receipt_path, state, manifest, manifest_path, run_dir
    )
    record_attempt(
        state, key, outcome, str(receipt_path.resolve()), receipt.get("reason"),
        dependencies, observation
    )
    save_state(run_dir, state)
    return key


def adapter_map(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise EvidenceError("--adapter must be SCENARIO_ID=/absolute/executable")
        scenario, executable = value.split("=", 1)
        path = Path(executable)
        if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
            raise EvidenceError(f"adapter is not an executable absolute path: {executable}")
        result[scenario] = path
    return result


def write_request(
    state: dict[str, Any], manifest: dict[str, Any], key: str, cell_dir: Path
) -> Path:
    scenario = scenario_map(manifest)[state["cells"][key]["scenario_id"]]
    request = {
        "schema": REQUEST_SCHEMA,
        "version": 1,
        "experiment_id": state["experiment_id"],
        "cell_key": key,
        "scenario": scenario,
        "mode": state["cells"][key]["mode"],
        "requested_dpr": state["cells"][key]["requested_dpr"],
        "expected_content_digest": source_digest(
            scenario, Path(state["manifest_path"]), state["plan"]["forge_sha"]
        ),
        "trial_contract": manifest["trial_contract"],
        "pulp_sha": state["plan"]["pulp_sha"],
        "forge_sha": state["plan"]["forge_sha"],
        "cell_directory": str(cell_dir.resolve()),
    }
    path = cell_dir / "request.json"
    atomic_json(path, request)
    return path


def terminate_adapter(process: subprocess.Popen[str]) -> tuple[str, str]:
    """Stop a timed-out adapter and collect its bounded diagnostic output."""
    try:
        if os.name == "posix":
            os.killpg(process.pid, 15)
        else:
            process.terminate()
    except ProcessLookupError:
        pass
    try:
        return process.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(process.pid, 9)
        else:
            process.kill()
        return process.communicate()


def run_cells(
    run_dir: Path, adapters: dict[str, Path], selectors: set[str], limit: int | None,
    timeout_seconds: float = 900,
) -> int:
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise EvidenceError("adapter timeout must be a positive finite number")
    if limit is not None and limit < 0:
        raise EvidenceError("cell limit must be non-negative")
    state = load_state(run_dir)
    manifest = load_json(Path(state["manifest_path"]))
    unknown_adapters = set(adapters) - set(scenario_map(manifest))
    if unknown_adapters:
        raise EvidenceError(f"adapters name unknown scenarios: {sorted(unknown_adapters)}")
    selected = [
        key for key, cell in state["cells"].items()
        if cell["status"] != "complete" and (not selectors or key in selectors)
    ]
    unknown = selectors - set(state["cells"])
    if unknown:
        raise EvidenceError(f"unknown cell selectors: {sorted(unknown)}")
    if limit is not None:
        selected = selected[:limit]
    for key in selected:
        state = load_state(run_dir)
        cell = state["cells"][key]
        cell_dir = cell_directory(run_dir, key)
        cell_dir.mkdir(parents=True, exist_ok=True)
        executable = adapters.get(cell["scenario_id"])
        if executable is None:
            record_attempt(
                state, key, "inconclusive", None,
                "scenario-specific adapter was not supplied",
                [f"adapter:{cell['scenario_id']}"], None,
            )
            save_state(run_dir, state)
            continue
        request = write_request(state, manifest, key, cell_dir)
        receipt = cell_dir / "receipt.json"
        process = subprocess.Popen(
            [str(executable), "--request", str(request), "--receipt", str(receipt)],
            cwd=cell_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            stdout, stderr = terminate_adapter(process)
            (cell_dir / "adapter.stdout.log").write_text(stdout, encoding="utf-8")
            (cell_dir / "adapter.stderr.log").write_text(stderr, encoding="utf-8")
            state = load_state(run_dir)
            record_attempt(
                state, key, "inconclusive", None,
                f"adapter exceeded the {timeout_seconds:g}s bounded runtime",
                [f"adapter:{cell['scenario_id']}:timeout"], None,
            )
            save_state(run_dir, state)
            continue
        completed = subprocess.CompletedProcess(
            process.args, process.returncode, stdout, stderr
        )
        (cell_dir / "adapter.stdout.log").write_text(completed.stdout, encoding="utf-8")
        (cell_dir / "adapter.stderr.log").write_text(completed.stderr, encoding="utf-8")
        if receipt.is_file():
            try:
                declared = load_json(receipt).get("outcome")
            except (OSError, json.JSONDecodeError, AttributeError) as error:
                state = load_state(run_dir)
                record_attempt(
                    state, key, "inconclusive", str(receipt.resolve()),
                    f"adapter receipt rejected: {error}", ["valid-cell-receipt"], None,
                )
                save_state(run_dir, state)
                continue
            expected_exit = {"pass": 0, "fail": 1, "skip": 2, "inconclusive": 3}.get(declared)
            if expected_exit is None or completed.returncode != expected_exit:
                state = load_state(run_dir)
                record_attempt(
                    state, key, "inconclusive", str(receipt.resolve()),
                    f"adapter exit {completed.returncode} disagrees with receipt outcome {declared}",
                    [f"adapter:{cell['scenario_id']}:exit-contract"], None,
                )
                save_state(run_dir, state)
                continue
            try:
                ingest_receipt(run_dir, receipt)
            except (EvidenceError, OSError, json.JSONDecodeError, KeyError, TypeError) as error:
                state = load_state(run_dir)
                record_attempt(
                    state, key, "inconclusive", str(receipt.resolve()),
                    f"adapter receipt rejected: {error}", ["valid-cell-receipt"], None,
                )
                save_state(run_dir, state)
        else:
            if completed.returncode == 2:
                outcome = "skip"
            else:
                outcome = "inconclusive"
            if completed.returncode < 0:
                reason = f"adapter terminated by signal {-completed.returncode}"
            else:
                reason = f"adapter exited {completed.returncode} without a receipt"
            state = load_state(run_dir)
            record_attempt(
                state, key, outcome, None, reason,
                [f"adapter:{cell['scenario_id']}:receipt"], None,
            )
            save_state(run_dir, state)
    return 0


def status_document(state: dict[str, Any]) -> dict[str, Any]:
    counts: dict[str, int] = {}
    dependencies: set[str] = set()
    for cell in state["cells"].values():
        counts[cell["status"]] = counts.get(cell["status"], 0) + 1
        dependencies.update(cell["dependencies"])
    complete = counts.get("complete", 0)
    return {
        "schema": "pulp.gpu-dpr-run-status.v1",
        "experiment_id": state["experiment_id"],
        "total_cells": len(state["cells"]),
        "complete_cells": complete,
        "incomplete_cells": len(state["cells"]) - complete,
        "status_counts": counts,
        "dependencies": sorted(dependencies),
        "result_status": project_result(state)["status"],
    }


def finalize(
    run_dir: Path, disposition: str, a2t: str, a3_budget: str, a3: str
) -> dict[str, Any]:
    state = load_state(run_dir)
    incomplete = [key for key, cell in state["cells"].items() if cell["status"] != "complete"]
    if incomplete:
        raise EvidenceError(f"cannot finalize: {len(incomplete)} matrix cells are incomplete")
    if not all(isinstance(value, str) and value for value in (a2t, a3_budget, a3)):
        raise EvidenceError("A2T receipt, A3 budget id, and A3 receipt must be non-empty")
    result = project_result(state)
    result["dependencies"] = {
        "a2t_receipt": a2t,
        "a3_budget_id": a3_budget,
        "a3_receipt": a3,
    }
    fidelity_passed = all(
        item["fidelity"][gate]
        for item in result["observations"]
        for gate in (
            "content_floor_passed", "small_text_legible",
            "thin_strokes_preserved", "logical_input_correct"
        )
    )
    if disposition != "no-change" and not fidelity_passed:
        raise EvidenceError("a B5 candidate cannot cross a failing fidelity/input oracle")
    result["status"] = "complete"
    result["disposition"] = disposition
    result["eligible_for_policy"] = fidelity_passed
    manifest = load_json(Path(state["manifest_path"]))
    schema_problems = experiment.json_schema_lite.validate(
        result,
        experiment.schema_for_lite(experiment.load_json(experiment.DEFAULT_SCHEMA)),
    )
    problems = [*schema_problems, *experiment.result_semantic_errors(
        result, manifest, experiment.canonical_sha256(manifest)
    )]
    if problems:
        raise EvidenceError("; ".join(problems))
    _, result_path, b5_path = run_paths(run_dir)
    atomic_json(result_path, result)
    if disposition == "no-change":
        b5 = {
            "schema": B5_SCHEMA,
            "a4_disposition": disposition,
            "status": "cancelled-no-change",
            "requires": [],
            "authorizes_policy_change": False,
        }
    else:
        b5 = {
            "schema": B5_SCHEMA,
            "a4_disposition": disposition,
            "status": "waiting-trigger",
            "requires": ["B0-adopted-vellum-api-refresh"],
            "authorizes_policy_change": False,
        }
        if disposition == "configured-max-candidate":
            b5["measured_candidate"] = {"configured_max_dpr": manifest["configured_max_dpr"]}
        else:
            b5["measured_candidate"] = {
                "adaptive_profile": manifest["adaptive_profile"]
            }
    atomic_json(b5_path, b5)
    return b5


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init")
    init.add_argument("--plan", type=Path, required=True)
    init.add_argument("--run-dir", type=Path, required=True)
    init.add_argument("--manifest", type=Path, default=experiment.DEFAULT_MANIFEST)
    run = sub.add_parser("run")
    run.add_argument("--run-dir", type=Path, required=True)
    run.add_argument("--adapter", action="append", default=[])
    run.add_argument("--cell", action="append", default=[])
    run.add_argument("--limit", type=int)
    run.add_argument("--timeout-seconds", type=float, default=900)
    ingest = sub.add_parser("ingest")
    ingest.add_argument("--run-dir", type=Path, required=True)
    ingest.add_argument("--receipt", type=Path, required=True)
    status = sub.add_parser("status")
    status.add_argument("--run-dir", type=Path, required=True)
    status.add_argument("--json", action="store_true")
    finish = sub.add_parser("finalize")
    finish.add_argument("--run-dir", type=Path, required=True)
    finish.add_argument("--disposition", choices=sorted(experiment.POLICY_DISPOSITIONS), required=True)
    finish.add_argument("--a2t-receipt", required=True)
    finish.add_argument("--a3-budget-id", required=True)
    finish.add_argument("--a3-receipt", required=True)
    args = parser.parse_args()
    try:
        if args.command == "init":
            manifest = load_json(args.manifest)
            problems = experiment.manifest_errors(manifest, args.manifest)
            if problems:
                raise EvidenceError("; ".join(problems))
            if args.run_dir.exists() and any(args.run_dir.iterdir()):
                raise EvidenceError("run directory must be absent or empty")
            args.run_dir.mkdir(parents=True, exist_ok=True)
            state = initial_state(load_json(args.plan), manifest, args.manifest)
            save_state(args.run_dir, state)
            print(f"gpu_dpr_run_initialized=true cells={len(state['cells'])}")
        elif args.command == "run":
            run_cells(
                args.run_dir, adapter_map(args.adapter), set(args.cell), args.limit,
                args.timeout_seconds,
            )
            print(json.dumps(status_document(load_state(args.run_dir)), sort_keys=True))
        elif args.command == "ingest":
            key = ingest_receipt(args.run_dir, args.receipt)
            print(f"gpu_dpr_cell_ingested=true cell={key}")
        elif args.command == "status":
            document = status_document(load_state(args.run_dir))
            if args.json:
                print(json.dumps(document, sort_keys=True, indent=2))
            else:
                print(
                    f"gpu_dpr_run_status={document['result_status']} "
                    f"complete={document['complete_cells']}/{document['total_cells']}"
                )
        else:
            b5 = finalize(
                args.run_dir, args.disposition, args.a2t_receipt,
                args.a3_budget_id, args.a3_receipt
            )
            print(json.dumps(b5, sort_keys=True))
    except (EvidenceError, OSError, json.JSONDecodeError, subprocess.TimeoutExpired) as error:
        print(f"gpu-dpr-runner: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
