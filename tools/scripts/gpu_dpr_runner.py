#!/usr/bin/env python3
"""Run and ingest resumable A4 DPR trials without selecting render policy."""

from __future__ import annotations

import argparse
import json
import math
import os
import secrets
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_contained_process as contained_process  # noqa: E402
import gpu_dpr_experiment as experiment  # noqa: E402
import gpu_dpr_v2_runner as runner_v2  # noqa: E402
from gpu_dpr_evidence import (  # noqa: E402
    A2T_RECEIPT_SCHEMA, A3_RECEIPT_SCHEMA, ALL_OUTCOMES, ARTIFACT_KINDS,
    COMPLETE_OUTCOMES, EvidenceError, INCOMPLETE_OUTCOMES, METRIC_UNITS,
    NONCE_HEX_LENGTH, RAW_SCHEMA, RECEIPT_SCHEMA, TRACE_ANALYSIS_SCHEMA,
    atomic_json, cell_directory, cell_key, checked_cell_directory,
    exact_executable, load_json, parse_cell_key, receipt_observation,
    regular_json, scenario_map, sha256_file, snapshot_file,
    snapshot_receipt_bundle, source_digest, valid_attempt_nonce,
    validated_dependency_receipts,
)

RUN_SCHEMA = "pulp.gpu-dpr-run-state.v1"
REQUEST_SCHEMA = "pulp.gpu-dpr-cell-request.v1"
B5_SCHEMA = "pulp.gpu-dpr-b5-gate.v1"
OUTPUT_CAP_BYTES = 1024 * 1024


class AdapterTerminationError(EvidenceError):
    """The DPR runner could not contain, terminate, and reap an adapter tree."""

    code = contained_process.ProcessTreeTerminationError.code


def initial_state(
    plan: dict[str, Any], manifest: dict[str, Any], manifest_path: Path,
    trace_analyzer: dict[str, str],
) -> dict[str, Any]:
    if plan.get("schema") != "pulp.gpu-dpr-experiment.v1":
        raise EvidenceError("plan is not a Pulp A4 DPR experiment document")
    if plan.get("status") != "planned" or plan.get("observations"):
        raise EvidenceError("runner initialization requires an empty planned result")
    similarity_minimum = manifest.get("trial_contract", {}).get(
        "capture_similarity_minimum"
    )
    if (
        isinstance(similarity_minimum, bool)
        or not isinstance(similarity_minimum, (int, float))
        or not 0 <= similarity_minimum <= 1
    ):
        raise EvidenceError("manifest lacks a ratified capture-similarity minimum")
    expected_digest = experiment.canonical_sha256(manifest)
    if plan["matrix"]["manifest_sha256"] != expected_digest:
        raise EvidenceError("plan and corpus manifest digests differ")
    if plan["matrix"]["scenario_ids"] != [item["id"] for item in manifest["scenarios"]]:
        raise EvidenceError("plan scenario ordering differs from the corpus")
    analyzer_path, analyzer_digest = exact_executable(
        trace_analyzer, "runner-pinned trace analyzer"
    )

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
        "trace_analyzer": {
            "path": str(analyzer_path.resolve()),
            "sha256": analyzer_digest,
        },
        "issued_attempts": {},
        "plan": plan,
        "cells": cells,
    }


def initialize_run(
    run_dir: Path, plan: dict[str, Any], manifest: dict[str, Any],
    manifest_path: Path, analyzer_source: Path,
) -> dict[str, Any]:
    if not analyzer_source.is_absolute():
        raise EvidenceError("trace analyzer must be an absolute executable path")
    pinned = executable_snapshot_path(
        run_dir / "tooling" / "trace-analyzer", analyzer_source
    )
    digest = snapshot_file(
        analyzer_source, pinned, "trace analyzer", executable=True,
    )
    return initial_state(
        plan, manifest, manifest_path,
        {"path": str(pinned.resolve()), "sha256": digest},
    )


def executable_snapshot_path(base: Path, source: Path) -> Path:
    """Keep the source executable suffix while choosing a confined snapshot name."""
    return base.with_name(base.name + source.suffix) if source.suffix else base


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


def record_attempt(
    state: dict[str, Any], key: str, outcome: str, receipt_path: str | None,
    reason: str | None, dependencies: list[str], observation: dict[str, Any] | None,
    nonce: str | None = None,
) -> None:
    if nonce is not None:
        state.setdefault("issued_attempts", {}).pop(nonce, None)
    cell = state["cells"][key]
    cell["attempts"].append({
        "number": len(cell["attempts"]) + 1,
        "outcome": outcome,
        "receipt": receipt_path,
        "reason": reason,
        "dependencies": dependencies,
        "nonce": nonce,
    })
    cell["dependencies"] = dependencies
    cell["observation"] = observation
    cell["status"] = "complete" if outcome in COMPLETE_OUTCOMES else outcome


def ingest_receipt(run_dir: Path, receipt_path: Path, attempt_nonce: str) -> str:
    if not valid_attempt_nonce(attempt_nonce):
        raise EvidenceError("ingest requires a valid runner attempt nonce")
    state = load_state(run_dir)
    manifest_path = Path(state["manifest_path"])
    manifest = load_json(manifest_path)
    receipt_path, nonce = snapshot_receipt_bundle(
        run_dir, state, receipt_path, attempt_nonce
    )
    receipt = regular_json(receipt_path, "snapshotted cell receipt")
    outcome = receipt.get("outcome", "inconclusive")
    key, observation, dependencies = receipt_observation(
        receipt_path, state, manifest, manifest_path, run_dir
    )
    record_attempt(
        state, key, outcome, str(receipt_path.resolve()), receipt.get("reason"),
        dependencies, observation, nonce
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
    state: dict[str, Any], manifest: dict[str, Any], key: str, cell_dir: Path,
    attempt_nonce: str,
) -> Path:
    scenario = scenario_map(manifest)[state["cells"][key]["scenario_id"]]
    request = {
        "schema": REQUEST_SCHEMA,
        "version": 1,
        "experiment_id": state["experiment_id"],
        "cell_key": key,
        "attempt_nonce": attempt_nonce,
        "attempt_number": len(state["cells"][key]["attempts"]) + 1,
        "scenario": scenario,
        "mode": state["cells"][key]["mode"],
        "requested_dpr": state["cells"][key]["requested_dpr"],
        "adaptive_profile": (
            manifest["adaptive_profile"]
            if state["cells"][key]["mode"] == "adaptive_simulation"
            else None
        ),
        "expected_content_digest": source_digest(
            scenario, Path(state["manifest_path"]), state["plan"]["forge_sha"]
        ),
        "trial_contract": manifest["trial_contract"],
        "pulp_sha": state["plan"]["pulp_sha"],
        "forge_sha": state["plan"]["forge_sha"],
        "pulp_source_root": str(experiment.ROOT.resolve()),
        "cell_directory": str(cell_dir.resolve()),
    }
    path = cell_dir / f"request-attempt-{attempt_nonce}.json"
    atomic_json(path, request)
    return path


def issue_attempt(
    run_dir: Path, state: dict[str, Any], manifest: dict[str, Any], key: str,
) -> tuple[str, Path]:
    if key not in state["cells"]:
        raise EvidenceError(f"unknown cell selector: {key}")
    cell_dir = checked_cell_directory(run_dir, key, create=True)
    nonce = secrets.token_hex(NONCE_HEX_LENGTH // 2)
    issued = state.setdefault("issued_attempts", {})
    for predecessor in [value for value, cell in issued.items() if cell == key]:
        del issued[predecessor]
    issued[nonce] = key
    request = write_request(state, manifest, key, cell_dir, nonce)
    save_state(run_dir, state)
    return nonce, request


def terminate_adapter(process: subprocess.Popen[bytes]) -> None:
    """Stop an adapter process tree without an unbounded post-kill wait."""
    try:
        contained_process.terminate_contained(process)
    except contained_process.ProcessTreeTerminationError as error:
        raise AdapterTerminationError(str(error)) from error


def spawn_adapter(command: list[str], *, cwd: Path) -> subprocess.Popen[bytes]:
    try:
        return contained_process.spawn_contained(command, cwd=cwd)
    except contained_process.ProcessTreeTerminationError as error:
        raise AdapterTerminationError(str(error)) from error


def communicate_bounded(
    process: subprocess.Popen[bytes], timeout_seconds: float,
) -> tuple[str, str, bool, bool]:
    """Drain both pipes concurrently with a strict one-MiB cap per stream."""
    buffers = [bytearray(), bytearray()]
    exceeded = threading.Event()

    def drain(index: int, stream: Any) -> None:
        while True:
            chunk = stream.read(65536)
            if not chunk:
                return
            remaining = OUTPUT_CAP_BYTES - len(buffers[index])
            buffers[index].extend(chunk[:max(remaining, 0)])
            if len(chunk) > remaining:
                exceeded.set()
                return

    threads = [
        threading.Thread(target=drain, args=(0, process.stdout), daemon=True),
        threading.Thread(target=drain, args=(1, process.stderr), daemon=True),
    ]
    for thread in threads:
        thread.start()
    deadline = time.monotonic() + timeout_seconds
    timed_out = False
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            timed_out = True
            break
        if exceeded.wait(min(0.05, remaining)):
            break
    terminate_adapter(process)
    for thread in threads:
        thread.join(timeout=2)
    if any(thread.is_alive() for thread in threads):
        raise AdapterTerminationError(
            f"{AdapterTerminationError.code}: adapter output pipes remained open "
            "after bounded process-tree termination"
        )
    return (
        buffers[0].decode("utf-8", errors="replace"),
        buffers[1].decode("utf-8", errors="replace"),
        timed_out,
        exceeded.is_set(),
    )


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
        cell_dir = checked_cell_directory(run_dir, key, create=True)
        executable = adapters.get(cell["scenario_id"])
        if executable is None:
            record_attempt(
                state, key, "inconclusive", None,
                "scenario-specific adapter was not supplied",
                [f"adapter:{cell['scenario_id']}"], None,
            )
            save_state(run_dir, state)
            continue
        attempt_nonce, request = issue_attempt(
            run_dir, state, manifest, key
        )
        pinned_adapter = executable_snapshot_path(
            run_dir / "tooling" / "adapters" / key / attempt_nonce,
            executable,
        )
        snapshot_file(
            executable, pinned_adapter, f"adapter:{cell['scenario_id']}",
            executable=True,
        )
        # Each attempt gets a new pathname and the legacy fixed receipt is
        # removed. A crashed/timeout adapter therefore cannot make a later
        # attempt ingest stale bytes that merely happen to remain in the cell.
        legacy_receipt = cell_dir / "receipt.json"
        if legacy_receipt.exists():
            legacy_receipt.unlink()
        receipt = cell_dir / f"receipt-attempt-{attempt_nonce}.json"
        if receipt.exists():
            receipt.unlink()
        process = spawn_adapter(
            [str(pinned_adapter), "--request", str(request), "--receipt", str(receipt)],
            cwd=cell_dir,
        )
        stdout, stderr, timed_out, output_exceeded = communicate_bounded(
            process, timeout_seconds
        )
        if timed_out or output_exceeded:
            (cell_dir / f"adapter-{attempt_nonce}.stdout.log").write_text(
                stdout, encoding="utf-8"
            )
            (cell_dir / f"adapter-{attempt_nonce}.stderr.log").write_text(
                stderr, encoding="utf-8"
            )
            state = load_state(run_dir)
            dependency = "output-limit" if output_exceeded else "timeout"
            record_attempt(
                state, key, "inconclusive", None,
                (f"adapter output exceeded {OUTPUT_CAP_BYTES} bytes per stream"
                 if output_exceeded else
                 f"adapter exceeded the {timeout_seconds:g}s bounded runtime"),
                [f"adapter:{cell['scenario_id']}:{dependency}"], None, attempt_nonce,
            )
            save_state(run_dir, state)
            continue
        completed = subprocess.CompletedProcess(
            process.args, process.returncode, stdout, stderr
        )
        (cell_dir / f"adapter-{attempt_nonce}.stdout.log").write_text(
            completed.stdout, encoding="utf-8"
        )
        (cell_dir / f"adapter-{attempt_nonce}.stderr.log").write_text(
            completed.stderr, encoding="utf-8"
        )
        if receipt.is_file():
            try:
                receipt_document = regular_json(receipt, "adapter cell receipt")
                declared = receipt_document.get("outcome")
                if receipt_document.get("attempt_nonce") != attempt_nonce:
                    raise EvidenceError("adapter receipt attempt nonce does not match request")
            except (EvidenceError, OSError, json.JSONDecodeError, AttributeError) as error:
                state = load_state(run_dir)
                record_attempt(
                    state, key, "inconclusive", str(receipt.resolve()),
                    f"adapter receipt rejected: {error}", ["valid-cell-receipt"], None,
                    attempt_nonce,
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
                    attempt_nonce,
                )
                save_state(run_dir, state)
                continue
            try:
                ingest_receipt(run_dir, receipt, attempt_nonce)
            except (
                EvidenceError, OSError, json.JSONDecodeError, KeyError, TypeError,
                subprocess.TimeoutExpired,
            ) as error:
                state = load_state(run_dir)
                record_attempt(
                    state, key, "inconclusive", str(receipt.resolve()),
                    f"adapter receipt rejected: {error}", ["valid-cell-receipt"], None,
                    attempt_nonce,
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
                attempt_nonce,
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


def revalidate_complete_state(
    run_dir: Path, state: dict[str, Any]
) -> dict[str, Any]:
    if experiment.canonical_sha256(state.get("plan")) != state.get("plan_sha256"):
        raise EvidenceError("embedded plan digest changed after initialization")
    manifest_path = Path(state["manifest_path"])
    manifest = load_json(manifest_path)
    problems = experiment.manifest_errors(manifest, manifest_path)
    if problems:
        raise EvidenceError("; ".join(problems))
    manifest_digest = experiment.canonical_sha256(manifest)
    if (
        manifest_digest != state.get("manifest_sha256")
        or state["plan"]["matrix"]["manifest_sha256"] != manifest_digest
    ):
        raise EvidenceError("manifest digest changed after initialization")

    for key, cell in state["cells"].items():
        if cell.get("status") != "complete" or not cell.get("attempts"):
            raise EvidenceError(f"cannot finalize: cell is incomplete: {key}")
        receipt_value = cell["attempts"][-1].get("receipt")
        if not isinstance(receipt_value, str) or not receipt_value:
            raise EvidenceError(f"complete cell lacks a durable receipt: {key}")
        derived_key, observation, dependencies = receipt_observation(
            Path(receipt_value), state, manifest, manifest_path, run_dir
        )
        if (
            derived_key != key
            or dependencies
            or observation is None
            or observation != cell.get("observation")
        ):
            raise EvidenceError(f"cell evidence changed after ingestion: {key}")
    return manifest


def policy_readiness(observations: list[dict[str, Any]], similarity_minimum: float) -> bool:
    fidelity_passed = all(
        (
            item["fidelity"][gate]
            if gate != "capture_similarity"
            else item["fidelity"][gate] >= similarity_minimum
        )
        for item in observations
        for gate in (
            "content_floor_passed", "capture_similarity", "small_text_legible",
            "thin_strokes_preserved", "logical_input_correct"
        )
    )
    all_metrics_available = all(
        statistic["provenance"] != "unavailable"
        for item in observations
        for statistic in item["metrics"].values()
    )
    return fidelity_passed and all_metrics_available


def b5_gate_document(disposition: str, manifest: dict[str, Any]) -> dict[str, Any]:
    """Project the inert B5 gate that follows an externally validated A4 result."""
    if disposition not in experiment.POLICY_DISPOSITIONS:
        raise EvidenceError(f"unsupported A4 disposition: {disposition}")
    if disposition == "no-change":
        return {
            "schema": B5_SCHEMA,
            "a4_disposition": disposition,
            "status": "cancelled-no-change",
            "requires": [],
            "authorizes_policy_change": False,
        }
    gate = {
        "schema": B5_SCHEMA,
        "a4_disposition": disposition,
        "status": "waiting-trigger",
        "requires": ["B0-adopted-vellum-api-refresh"],
        "authorizes_policy_change": False,
    }
    if disposition == "configured-max-candidate":
        gate["measured_candidate"] = {
            "configured_max_dpr": manifest["configured_max_dpr"]
        }
    else:
        gate["measured_candidate"] = {
            "adaptive_profile": manifest["adaptive_profile"]
        }
    return gate


def finalize(
    run_dir: Path, disposition: str, a2t: str, a3_budget: str, a3: str
) -> dict[str, Any]:
    state = load_state(run_dir)
    dependencies = validated_dependency_receipts(
        a2t, a3_budget, a3, state["plan"]
    )
    manifest = revalidate_complete_state(run_dir, state)
    result = project_result(state)
    result["dependencies"] = dependencies
    similarity_minimum = manifest["trial_contract"]["capture_similarity_minimum"]
    policy_ready = policy_readiness(result["observations"], similarity_minimum)
    if disposition != "no-change" and not policy_ready:
        raise EvidenceError(
            "a B5 candidate cannot cross a failing fidelity/input oracle "
            "or unavailable required metric"
        )
    result["status"] = "complete"
    result["disposition"] = disposition
    result["eligible_for_policy"] = policy_ready
    schema_problems = experiment.json_schema_lite.validate(
        result,
        experiment.schema_for_lite(experiment.load_json(experiment.DEFAULT_SCHEMA_V1)),
    )
    problems = [*schema_problems, *experiment.result_semantic_errors(
        result, manifest, experiment.canonical_sha256(manifest)
    )]
    if problems:
        raise EvidenceError("; ".join(problems))
    _, result_path, b5_path = run_paths(run_dir)
    atomic_json(result_path, result)
    b5 = b5_gate_document(disposition, manifest)
    atomic_json(b5_path, b5)
    return b5


def finalize_v2(run_dir: Path) -> dict[str, Any]:
    """Compatibility wrapper around receipt-derived v2 finalization."""
    return runner_v2.finalize(run_dir)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init")
    init.add_argument("--plan", type=Path, required=True)
    init.add_argument("--run-dir", type=Path, required=True)
    init.add_argument("--manifest", type=Path, default=experiment.DEFAULT_MANIFEST)
    init.add_argument("--trace-analyzer", type=Path, required=True)
    run = sub.add_parser("run")
    run.add_argument("--run-dir", type=Path, required=True)
    run.add_argument("--adapter", action="append", default=[])
    run.add_argument("--cell", action="append", default=[])
    run.add_argument("--limit", type=int)
    run.add_argument("--timeout-seconds", type=float, default=900)
    issue = sub.add_parser("issue")
    issue.add_argument("--run-dir", type=Path, required=True)
    issue.add_argument("--cell", required=True)
    ingest = sub.add_parser("ingest")
    ingest.add_argument("--run-dir", type=Path, required=True)
    ingest.add_argument("--receipt", type=Path, required=True)
    ingest.add_argument("--attempt-nonce", required=True)
    status = sub.add_parser("status")
    status.add_argument("--run-dir", type=Path, required=True)
    status.add_argument("--json", action="store_true")
    finish = sub.add_parser("finalize-v1")
    finish.add_argument("--run-dir", type=Path, required=True)
    finish.add_argument("--disposition", choices=sorted(experiment.POLICY_DISPOSITIONS), required=True)
    finish.add_argument("--a2t-receipt", required=True)
    finish.add_argument("--a3-budget-id", required=True)
    finish.add_argument("--a3-receipt", required=True)
    init_v2 = sub.add_parser("init-v2")
    init_v2.add_argument("--run-dir", type=Path, required=True)
    init_v2.add_argument("--experiment-id", required=True)
    init_v2.add_argument("--trace-analyzer", type=Path, required=True)
    run_v2 = sub.add_parser("run-v2")
    run_v2.add_argument("--run-dir", type=Path, required=True)
    run_v2.add_argument("--cell", required=True)
    run_v2.add_argument("--adapter", type=Path, required=True)
    run_v2.add_argument("--timeout-seconds", type=float, default=900)
    status_v2 = sub.add_parser("status-v2")
    status_v2.add_argument("--run-dir", type=Path, required=True)
    finish_v2 = sub.add_parser("finalize-v2")
    finish_v2.add_argument("--run-dir", type=Path, required=True)
    verify_v2 = sub.add_parser("verify-live-v2")
    verify_v2.add_argument("--evidence-root", type=Path, required=True)
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
            state = initialize_run(
                args.run_dir, load_json(args.plan), manifest, args.manifest,
                args.trace_analyzer,
            )
            save_state(args.run_dir, state)
            print(f"gpu_dpr_run_initialized=true cells={len(state['cells'])}")
        elif args.command == "run":
            run_cells(
                args.run_dir, adapter_map(args.adapter), set(args.cell), args.limit,
                args.timeout_seconds,
            )
            print(json.dumps(status_document(load_state(args.run_dir)), sort_keys=True))
        elif args.command == "issue":
            state = load_state(args.run_dir)
            manifest = load_json(Path(state["manifest_path"]))
            nonce, request = issue_attempt(
                args.run_dir, state, manifest, args.cell
            )
            print(json.dumps({
                "attempt_nonce": nonce,
                "request": str(request.resolve()),
            }, sort_keys=True))
        elif args.command == "ingest":
            key = ingest_receipt(
                args.run_dir, args.receipt, args.attempt_nonce
            )
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
        elif args.command == "finalize-v1":
            b5 = finalize(
                args.run_dir, args.disposition, args.a2t_receipt,
                args.a3_budget_id, args.a3_receipt
            )
            print(json.dumps(b5, sort_keys=True))
        elif args.command == "init-v2":
            state = runner_v2.initialize(
                args.run_dir, args.experiment_id, args.trace_analyzer
            )
            print(
                f"gpu_dpr_v2_run_initialized=true cells={len(state['cells'])}"
            )
        elif args.command == "run-v2":
            accepted = runner_v2.run_one(
                args.run_dir, args.cell, args.adapter, args.timeout_seconds
            )
            print(json.dumps({
                "gpu_dpr_v2_cell_ingested": True,
                "cell": accepted["cell_key"],
            }, sort_keys=True))
        elif args.command == "status-v2":
            print(json.dumps(runner_v2.status(args.run_dir), sort_keys=True, indent=2))
        elif args.command == "finalize-v2":
            result = finalize_v2(args.run_dir)
            print(json.dumps(result["b5_gate"], sort_keys=True))
        else:
            receipt = experiment.emit_live_verification(args.evidence_root)
            print(json.dumps(receipt, sort_keys=True))
    except (
        EvidenceError, runner_v2.V2RunnerError, OSError, ValueError,
        json.JSONDecodeError, subprocess.TimeoutExpired,
    ) as error:
        print(f"gpu-dpr-runner: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
