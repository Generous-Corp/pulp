#!/usr/bin/env python3
"""Nonce-bound runner and terminal finalizer for A4 DPR v2."""

from __future__ import annotations

import json
import math
import os
import secrets
import shutil
import stat
import subprocess
from pathlib import Path
from typing import Any

import gpu_dpr_experiment as experiment
import gpu_dpr_v2_evidence as evidence
import gpu_dpr_v2_terminal as terminal


REQUEST_SCHEMA = "pulp.gpu-dpr-cell-request.v2"
PRODUCER_RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-producer-receipt.v2"
ACCEPTED_RECEIPT_SCHEMA = evidence.CELL_RECEIPT_SCHEMA
AUTHORIZED_MANIFEST = "authorized-manifest-v2.json"
RESULT = "result-v2.json"
MAX_RECEIPT_BYTES = 16 * 1024 * 1024
OUTPUT_CAP_BYTES = 1024 * 1024
EXIT_BY_OUTCOME = {"pass": 0, "fail": 1}


class V2RunnerError(ValueError):
    """The v2 runner cannot prove an exact collection/finalization step."""


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, indent=2).encode("utf-8") + b"\n"


def _write_new_json(path: Path, value: Any, mode: int = 0o400) -> None:
    if path.exists() or path.is_symlink():
        raise V2RunnerError(f"runner file already exists: {path}")
    evidence._atomic_bytes(path, _json_bytes(value), mode)


def run_identity(run_dir: Path) -> dict[str, int]:
    root = evidence._checked_root(run_dir, "A4 v2 runner")
    metadata = os.lstat(root)
    return {
        "device": metadata.st_dev,
        "inode": metadata.st_ino,
        "owner": metadata.st_uid,
    }


def run_cell_key(campaign: str, scenario: str, mode: str, dpr: float) -> str:
    token = str(int(dpr)) if float(dpr).is_integer() else format(float(dpr), ".15g")
    return f"{campaign}__{scenario}__{mode}__dpr-{token}"


def _expected_cells(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    rows: dict[str, dict[str, Any]] = {}
    for campaign in ("original", "repeat"):
        for scenario in manifest["scenarios"]:
            for mode in experiment.MODES:
                for requested_dpr in experiment.REQUESTED_DPRS:
                    key = run_cell_key(campaign, scenario["id"], mode, requested_dpr)
                    rows[key] = {
                        "campaign": campaign,
                        "scenario_id": scenario["id"],
                        "mode": mode,
                        "requested_dpr": requested_dpr,
                        "status": "pending",
                        "attempts": [],
                        "issued": None,
                        "accepted_receipt": None,
                    }
    return rows


def _live_authority() -> tuple[str, dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    live, errors = experiment.live_protected_main_state()
    if errors or live is None:
        raise V2RunnerError("; ".join(errors))
    documents, blobs = terminal.validate_dependencies(
        experiment.ROOT, live["head"], experiment.verify_remote_product_policy
    )
    return live["head"], documents, blobs


def initialize(
    run_dir: Path, experiment_id: str, trace_analyzer_source: Path,
) -> dict[str, Any]:
    """Create an absent runner root from canonical terminal dependencies only."""
    if not run_dir.is_absolute():
        raise V2RunnerError("A4 v2 run directory must be absolute")
    if run_dir.exists() or run_dir.is_symlink():
        raise V2RunnerError("A4 v2 runner must create an absent run directory")
    if not isinstance(experiment_id, str) or not experiment_id or len(experiment_id) > 96:
        raise V2RunnerError("A4 v2 experiment id is invalid")
    run_dir.mkdir(mode=0o700, parents=True)
    try:
        evidence.initialize_run_key(run_dir)
        live_head, documents, blobs = _live_authority()
        _, analyzer_digest, analyzer_bytes = _checked_adapter(trace_analyzer_source)
        expected_analyzer = documents[terminal.A2T_ID]["payload"][
            "trace_analyzer_sha256"
        ]
        if analyzer_digest != expected_analyzer:
            raise V2RunnerError(
                "trace analyzer bytes differ from terminal A2T authority"
            )
        analyzer_suffix = trace_analyzer_source.suffix or ".bin"
        analyzer_path = run_dir / "tooling" / f"trace-analyzer{analyzer_suffix}"
        evidence.snapshot_regular(
            trace_analyzer_source.parent, trace_analyzer_source.name, analyzer_path,
            "terminal A2T trace analyzer", max_bytes=128 * 1024 * 1024,
            expected_sha256=analyzer_digest, expected_bytes=analyzer_bytes,
            executable=True,
        )
        analyzer_descriptor = {
            "path": analyzer_path.relative_to(run_dir).as_posix(),
            "sha256": analyzer_digest,
            "bytes": analyzer_bytes,
        }
        _write_new_json(
            run_dir / evidence.TRACE_ANALYZER_DESCRIPTOR, analyzer_descriptor
        )
        blocked = experiment.load_json(experiment.ROOT / terminal.BLOCKED_MANIFEST_TEMPLATE)
        manifest = terminal.derive_manifest(blocked, documents)
        problems = experiment.manifest_errors(
            manifest, experiment.ROOT / terminal.CANONICAL_MANIFEST
        )
        if problems:
            raise V2RunnerError("; ".join(problems))
        manifest_path = run_dir / AUTHORIZED_MANIFEST
        _write_new_json(manifest_path, manifest)
        dependencies = terminal.dependency_projection(documents, blobs)
        dependency_snapshots: dict[str, dict[str, Any]] = {}
        for receipt_id, projection in dependencies.items():
            destination = run_dir / "dependencies" / f"{receipt_id}.json"
            digest, byte_count = evidence.snapshot_regular(
                experiment.ROOT, projection["path"], destination,
                f"terminal dependency {receipt_id}", max_bytes=MAX_RECEIPT_BYTES,
                expected_sha256=blobs[receipt_id]["sha256"],
                expected_bytes=blobs[receipt_id]["bytes"],
            )
            dependency_snapshots[receipt_id] = {
                **projection,
                "snapshot_path": destination.relative_to(run_dir).as_posix(),
                "file_sha256": digest,
                "bytes": byte_count,
            }
        installed = documents[terminal.A3_ID]["installed_revisions"]
        state = {
            "schema": evidence.RUN_STATE_SCHEMA,
            "version": 2,
            "run_id": secrets.token_hex(32),
            "root_identity": run_identity(run_dir),
            "initialized_protected_head": live_head,
            "experiment_id": experiment_id,
            "plan_revision": live_head,
            "pulp_sha": installed["pulp"],
            "forge_sha": installed["forge"],
            "installed_revisions": installed,
            "trace_analyzer": analyzer_descriptor,
            "manifest": {
                "path": AUTHORIZED_MANIFEST,
                "sha256": experiment.canonical_sha256(manifest),
            },
            "dependencies": dependency_snapshots,
            "cells": _expected_cells(manifest),
        }
        evidence.write_integrity_state(run_dir, state)
        return state
    except Exception:
        # An initialization failure has issued no attempt and retained no user
        # evidence.  Remove only the exact directory this call just created.
        shutil.rmtree(run_dir)
        raise


def load_state(run_dir: Path) -> tuple[dict[str, Any], str]:
    state, digest = evidence.read_integrity_state(run_dir)
    if state.get("root_identity") != run_identity(run_dir):
        raise V2RunnerError("A4 v2 run directory identity differs from initialization")
    if not isinstance(state.get("run_id"), str) or not evidence._is_lower_hex(
        state["run_id"], 64
    ):
        raise V2RunnerError("A4 v2 runner id is malformed")
    return state, digest


def save_state(run_dir: Path, state: dict[str, Any]) -> str:
    if state.get("root_identity") != run_identity(run_dir):
        raise V2RunnerError("refusing to write state in a substituted run directory")
    return evidence.write_integrity_state(run_dir, state)


def load_manifest(run_dir: Path, state: dict[str, Any]) -> dict[str, Any]:
    manifest, _, _ = evidence.regular_json(
        run_dir, state["manifest"]["path"], "runner-derived A4 v2 manifest"
    )
    if experiment.canonical_sha256(manifest) != state["manifest"]["sha256"]:
        raise V2RunnerError("runner-derived manifest bytes changed")
    return manifest


def _checked_adapter(source: Path) -> tuple[Path, str, int]:
    if not source.is_absolute() or source.is_symlink():
        raise V2RunnerError("v2 adapter must be an absolute non-symlink executable")
    metadata = os.lstat(source)
    if not stat.S_ISREG(metadata.st_mode) or not metadata.st_mode & 0o111:
        raise V2RunnerError("v2 adapter must be an executable regular file")
    digest, size, _ = evidence.file_identity(
        source, "v2 measurement adapter", max_bytes=128 * 1024 * 1024
    )
    return source, digest, size


def issue(
    run_dir: Path, key: str, adapter_source: Path,
) -> tuple[dict[str, Any], Path]:
    state, _ = load_state(run_dir)
    if key not in state["cells"]:
        raise V2RunnerError(f"unknown A4 v2 cell: {key}")
    cell_state = state["cells"][key]
    if cell_state["status"] == "complete" or cell_state.get("issued") is not None:
        raise V2RunnerError(f"A4 v2 cell is complete or already issued: {key}")
    _, adapter_digest, adapter_bytes = _checked_adapter(adapter_source)
    nonce = secrets.token_hex(16)
    attempt = len(cell_state["attempts"]) + 1
    attempt_root = run_dir / "attempts" / key / nonce
    adapter_suffix = adapter_source.suffix if adapter_source.suffix else ".bin"
    adapter_path = attempt_root / f"adapter{adapter_suffix}"
    adapter_path.parent.mkdir(parents=True, mode=0o700)
    evidence.snapshot_regular(
        adapter_source.parent, adapter_source.name, adapter_path,
        "v2 measurement adapter", max_bytes=128 * 1024 * 1024,
        expected_sha256=adapter_digest, expected_bytes=adapter_bytes, executable=True,
    )
    request = {
        "schema": REQUEST_SCHEMA,
        "version": 2,
        "run_id": state["run_id"],
        "cell_key": key,
        "attempt_nonce": nonce,
        "attempt_number": attempt,
        "expected_cell": {
            field: cell_state[field]
            for field in ("campaign", "scenario_id", "mode", "requested_dpr")
        },
        "manifest_sha256": state["manifest"]["sha256"],
        "installed_revisions": state["installed_revisions"],
        "adapter": {
            "path": adapter_path.relative_to(run_dir).as_posix(),
            "sha256": adapter_digest,
            "bytes": adapter_bytes,
        },
        "output_directory": "producer-output",
    }
    request_path = attempt_root / "request.json"
    _write_new_json(request_path, request)
    request_digest, request_bytes, _ = evidence.file_identity(
        request_path, "v2 request", max_bytes=MAX_RECEIPT_BYTES
    )
    cell_state["issued"] = {
        "nonce": nonce,
        "request_path": request_path.relative_to(run_dir).as_posix(),
        "request_sha256": request_digest,
        "request_bytes": request_bytes,
        "adapter_path": adapter_path.relative_to(run_dir).as_posix(),
        "adapter_sha256": adapter_digest,
        "adapter_bytes": adapter_bytes,
    }
    save_state(run_dir, state)
    return request, request_path


def _read_producer_receipt(receipt_root: Path) -> tuple[dict[str, Any], str, int]:
    receipt, digest, size = evidence.regular_json(
        receipt_root, "receipt.json", "v2 producer receipt"
    )
    if set(receipt) != {
        "schema", "version", "run_id", "cell_key", "attempt_nonce",
        "request_sha256", "cell",
    } or receipt.get("schema") != PRODUCER_RECEIPT_SCHEMA or receipt.get("version") != 2:
        raise V2RunnerError("v2 producer receipt differs from the closed contract")
    if size > MAX_RECEIPT_BYTES:
        raise V2RunnerError("v2 producer receipt exceeds its byte bound")
    return receipt, digest, size


def _artifact_suffix(kind: str) -> str:
    if kind in {"capture", "reference_capture"}:
        return ".png"
    if kind == "trace":
        return ".pftrace"
    return ".bin" if kind == "product" else ".json"


def ingest_completed_attempt(
    run_dir: Path, key: str, nonce: str, producer_pid: int, exit_code: int,
) -> dict[str, Any]:
    """Snapshot/rederive one receipt created by an adapter the runner invoked."""
    state, _ = load_state(run_dir)
    cell_state = state["cells"].get(key)
    issued = cell_state.get("issued") if isinstance(cell_state, dict) else None
    if not isinstance(issued, dict) or issued.get("nonce") != nonce:
        raise V2RunnerError("producer receipt does not match one live runner-issued attempt")
    attempt_root = run_dir / "attempts" / key / nonce
    producer_root = attempt_root / "producer-output"
    receipt, receipt_digest, receipt_bytes = _read_producer_receipt(producer_root)
    if (
        receipt["run_id"] != state["run_id"]
        or receipt["cell_key"] != key
        or receipt["attempt_nonce"] != nonce
        or receipt["request_sha256"] != issued["request_sha256"]
    ):
        raise V2RunnerError("producer receipt is not bound to its runner request")
    cell = receipt.get("cell")
    if not isinstance(cell, dict):
        raise V2RunnerError("producer receipt lacks one v2 cell")
    expected = {
        field: cell_state[field]
        for field in ("campaign", "scenario_id", "mode", "requested_dpr")
    }
    if any(cell.get(field) != value for field, value in expected.items()):
        raise V2RunnerError("producer receipt names a different matrix cell")
    if cell.get("attempt_nonce") != nonce:
        raise V2RunnerError("cell body is not bound to the runner attempt nonce")
    identity = cell.get("identity")
    if not isinstance(identity, dict) or identity.get("adapter_sha256") != issued["adapter_sha256"]:
        raise V2RunnerError("cell identity differs from the executed adapter bytes")
    forge_cell = cell["scenario_id"].startswith("forge-")
    expected_build = state["installed_revisions"]["forge" if forge_cell else "pulp"]
    if expected_build is None or identity.get("build_sha") != expected_build:
        raise V2RunnerError("cell build identity differs from installed terminal authority")
    declared_artifacts = cell.get("artifacts")
    if not isinstance(declared_artifacts, list):
        raise V2RunnerError("producer cell lacks artifact inventory")
    snapshot_root = run_dir / "snapshots" / key / nonce
    rewritten: list[dict[str, Any]] = []
    for artifact in declared_artifacts:
        if not isinstance(artifact, dict) or set(artifact) != evidence.ARTIFACT_FIELDS:
            raise V2RunnerError("producer artifact differs from the closed descriptor")
        kind = artifact.get("kind")
        if kind not in evidence.ARTIFACT_KINDS:
            raise V2RunnerError("producer artifact kind is unsupported")
        destination = snapshot_root / f"{kind}{_artifact_suffix(kind)}"
        max_bytes = (
            evidence.MAX_CAPTURE_BYTES if kind in {"capture", "reference_capture"}
            else evidence.MAX_TRACE_BYTES if kind == "trace"
            else evidence.MAX_PRODUCT_BYTES if kind == "product"
            else evidence.MAX_JSON_BYTES
        )
        evidence.snapshot_regular(
            producer_root, artifact["path"], destination, f"producer {kind}",
            max_bytes=max_bytes, expected_sha256=artifact["sha256"],
            expected_bytes=artifact["bytes"], executable=kind == "product",
        )
        rewritten.append({
            **artifact,
            "path": destination.relative_to(run_dir).as_posix(),
        })
    cell = json.loads(json.dumps(cell))
    cell["artifacts"] = rewritten
    manifest = load_manifest(run_dir, state)
    analyzer = evidence.trace_analyzer_identity(
        run_dir, state["trace_analyzer"]["sha256"]
    )
    evidence.validate_cell_artifacts(cell, manifest, run_dir, analyzer)
    producer_snapshot = snapshot_root / "producer-receipt.json"
    evidence.snapshot_regular(
        producer_root, "receipt.json", producer_snapshot, "v2 producer receipt",
        max_bytes=MAX_RECEIPT_BYTES, expected_sha256=receipt_digest,
        expected_bytes=receipt_bytes,
    )
    accepted = {
        "schema": ACCEPTED_RECEIPT_SCHEMA,
        "version": 2,
        "run_id": state["run_id"],
        "cell_key": key,
        "attempt_nonce": nonce,
        "request": {
            "path": issued["request_path"],
            "sha256": issued["request_sha256"],
            "bytes": issued["request_bytes"],
        },
        "adapter": {
            "path": issued["adapter_path"],
            "sha256": issued["adapter_sha256"],
            "bytes": issued["adapter_bytes"],
            "process_id": producer_pid,
            "exit_code": exit_code,
        },
        "producer_receipt": {
            "path": producer_snapshot.relative_to(run_dir).as_posix(),
            "sha256": receipt_digest,
            "bytes": receipt_bytes,
        },
        "cell": cell,
    }
    accepted_path = snapshot_root / "accepted-receipt.json"
    _write_new_json(accepted_path, accepted)
    accepted_digest = evidence.canonical_sha256(accepted)
    cell_state["attempts"].append({
        "number": len(cell_state["attempts"]) + 1,
        "nonce": nonce,
        "outcome": cell.get("outcome"),
        "accepted_receipt_sha256": accepted_digest,
    })
    cell_state["issued"] = None
    if cell.get("outcome") == "pass" and exit_code == 0:
        cell_state["status"] = "complete"
        cell_state["accepted_receipt"] = {
            "path": accepted_path.relative_to(run_dir).as_posix(),
            "sha256": accepted_digest,
        }
    else:
        cell_state["status"] = "failed"
        cell_state["accepted_receipt"] = None
    save_state(run_dir, state)
    return accepted


def run_one(
    run_dir: Path, key: str, adapter_source: Path, timeout_seconds: float,
) -> dict[str, Any]:
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise V2RunnerError("v2 adapter timeout must be positive and finite")
    request, request_path = issue(run_dir, key, adapter_source)
    nonce = request["attempt_nonce"]
    state, _ = load_state(run_dir)
    adapter = run_dir / state["cells"][key]["issued"]["adapter_path"]
    producer_root = request_path.parent / request["output_directory"]
    producer_root.mkdir(mode=0o700)
    process = subprocess.Popen(
        [str(adapter), "--request", str(request_path), "--output", str(producer_root)],
        cwd=request_path.parent, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise V2RunnerError("v2 measurement adapter exceeded its bounded runtime")
    if len(stdout) > OUTPUT_CAP_BYTES or len(stderr) > OUTPUT_CAP_BYTES:
        raise V2RunnerError("v2 measurement adapter exceeded its output bound")
    receipt, _, _ = _read_producer_receipt(producer_root)
    expected_exit = EXIT_BY_OUTCOME.get(receipt.get("cell", {}).get("outcome"))
    if expected_exit is None or process.returncode != expected_exit:
        raise V2RunnerError("v2 adapter exit code differs from its receipt outcome")
    return ingest_completed_attempt(run_dir, key, nonce, process.pid, process.returncode)


def _exact_json_file(
    run_dir: Path, descriptor: dict[str, Any], label: str,
) -> dict[str, Any]:
    if not isinstance(descriptor, dict) or set(descriptor) != {"path", "sha256"}:
        raise V2RunnerError(f"{label} descriptor is malformed")
    document, _, _ = evidence.regular_json(run_dir, descriptor["path"], label)
    if evidence.canonical_sha256(document) != descriptor["sha256"]:
        raise V2RunnerError(f"{label} canonical digest changed")
    return document


def rederive_cell(
    run_dir: Path, state: dict[str, Any], key: str, manifest: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    cell_state = state["cells"][key]
    if cell_state.get("status") != "complete":
        raise V2RunnerError(f"cannot finalize incomplete v2 cell: {key}")
    accepted = _exact_json_file(
        run_dir, cell_state.get("accepted_receipt"), f"accepted receipt {key}"
    )
    required_fields = {
        "schema", "version", "run_id", "cell_key", "attempt_nonce", "request",
        "adapter", "producer_receipt", "cell",
    }
    if (
        set(accepted) != required_fields
        or accepted.get("schema") != ACCEPTED_RECEIPT_SCHEMA
        or accepted.get("version") != 2
        or accepted.get("run_id") != state["run_id"]
        or accepted.get("cell_key") != key
    ):
        raise V2RunnerError(f"accepted receipt identity differs: {key}")
    expected_accepted_path = (
        Path("snapshots") / key / accepted["attempt_nonce"] / "accepted-receipt.json"
    ).as_posix()
    if cell_state["accepted_receipt"]["path"] != expected_accepted_path:
        raise V2RunnerError(f"accepted receipt path is substituted: {key}")
    request_descriptor = accepted["request"]
    request, request_file_sha, request_bytes = evidence.regular_json(
        run_dir, request_descriptor["path"], f"request {key}"
    )
    expected_request_path = (
        Path("attempts") / key / accepted["attempt_nonce"] / "request.json"
    ).as_posix()
    if (
        set(request) != {
            "schema", "version", "run_id", "cell_key", "attempt_nonce",
            "attempt_number", "expected_cell", "manifest_sha256",
            "installed_revisions", "adapter", "output_directory",
        }
        or request.get("schema") != REQUEST_SCHEMA
        or request.get("version") != 2
        or request_descriptor.get("path") != expected_request_path
        or request_file_sha != request_descriptor["sha256"]
        or request_bytes != request_descriptor["bytes"]
        or request.get("run_id") != state["run_id"]
        or request.get("cell_key") != key
        or request.get("attempt_nonce") != accepted["attempt_nonce"]
        or request.get("manifest_sha256") != state["manifest"]["sha256"]
        or request.get("installed_revisions") != state["installed_revisions"]
        or request.get("output_directory") != "producer-output"
        or request.get("expected_cell") != {
            field: cell_state[field]
            for field in ("campaign", "scenario_id", "mode", "requested_dpr")
        }
        or request.get("adapter") != {
            field: accepted["adapter"][field]
            for field in ("path", "sha256", "bytes")
        }
    ):
        raise V2RunnerError(f"accepted receipt request binding changed: {key}")
    adapter = accepted["adapter"]
    expected_adapter_path = request["adapter"].get("path")
    adapter_path = evidence.checked_regular_path(
        run_dir, adapter["path"], f"adapter snapshot {key}"
    )
    adapter_sha, adapter_bytes, _ = evidence.file_identity(
        adapter_path, f"adapter snapshot {key}", max_bytes=128 * 1024 * 1024
    )
    if (
        set(adapter) != {
            "path", "sha256", "bytes", "process_id", "exit_code"
        }
        or adapter.get("path") != expected_adapter_path
        or adapter.get("sha256") != request["adapter"].get("sha256")
        or adapter.get("bytes") != request["adapter"].get("bytes")
        or adapter_sha != adapter["sha256"] or adapter_bytes != adapter["bytes"]
        or adapter.get("exit_code") != 0
        or isinstance(adapter.get("process_id"), bool)
        or not isinstance(adapter.get("process_id"), int)
        or adapter["process_id"] <= 0
    ):
        raise V2RunnerError(f"accepted adapter execution identity changed: {key}")
    producer_descriptor = accepted["producer_receipt"]
    expected_producer_path = (
        Path("snapshots") / key / accepted["attempt_nonce"]
        / "producer-receipt.json"
    ).as_posix()
    producer, producer_sha, producer_bytes = evidence.regular_json(
        run_dir, producer_descriptor["path"], f"producer receipt {key}"
    )
    if (
        set(producer_descriptor) != {"path", "sha256", "bytes"}
        or producer_descriptor.get("path") != expected_producer_path
        or producer_sha != producer_descriptor["sha256"]
        or producer_bytes != producer_descriptor["bytes"]
        or producer.get("run_id") != state["run_id"]
        or producer.get("cell_key") != key
        or producer.get("attempt_nonce") != accepted["attempt_nonce"]
        or producer.get("request_sha256") != request_descriptor["sha256"]
    ):
        raise V2RunnerError(f"producer receipt snapshot changed: {key}")
    source_cell = producer.get("cell")
    derived_cell = json.loads(json.dumps(source_cell))
    accepted_cell = accepted.get("cell")
    if not isinstance(derived_cell, dict) or not isinstance(accepted_cell, dict):
        raise V2RunnerError(f"accepted receipt lacks a cell: {key}")
    source_artifacts = {item["kind"]: item for item in derived_cell.get("artifacts", [])}
    accepted_artifacts = {item["kind"]: item for item in accepted_cell.get("artifacts", [])}
    if set(source_artifacts) != evidence.ARTIFACT_KINDS or set(accepted_artifacts) != evidence.ARTIFACT_KINDS:
        raise V2RunnerError(f"accepted artifact inventory differs: {key}")
    rewritten = []
    for kind in sorted(evidence.ARTIFACT_KINDS):
        source = source_artifacts[kind]
        retained = accepted_artifacts[kind]
        if {field: source[field] for field in evidence.ARTIFACT_FIELDS - {"path"}} != {
            field: retained[field] for field in evidence.ARTIFACT_FIELDS - {"path"}
        }:
            raise V2RunnerError(f"accepted {kind} metadata differs from producer: {key}")
        expected_path = (
            Path("snapshots") / key / accepted["attempt_nonce"]
            / f"{kind}{_artifact_suffix(kind)}"
        ).as_posix()
        if retained["path"] != expected_path:
            raise V2RunnerError(f"accepted {kind} path is substituted: {key}")
        rewritten.append(retained)
    derived_cell["artifacts"] = rewritten
    if derived_cell != accepted_cell:
        raise V2RunnerError(f"accepted cell differs from producer receipt: {key}")
    analyzer = evidence.trace_analyzer_identity(
        run_dir, state["trace_analyzer"]["sha256"]
    )
    evidence.validate_cell_artifacts(accepted_cell, manifest, run_dir, analyzer)
    return accepted_cell, evidence.canonical_sha256(accepted)


def finalize(run_dir: Path) -> dict[str, Any]:
    """Derive the terminal candidate only from retained receipts and live dependencies."""
    state, _ = load_state(run_dir)
    live_head, documents, blobs = _live_authority()
    current_projection = terminal.dependency_projection(documents, blobs)
    for receipt_id, projection in current_projection.items():
        saved = state["dependencies"].get(receipt_id, {})
        if any(saved.get(field) != projection[field] for field in ("path", "sha256", "blob")):
            raise V2RunnerError("terminal dependency changed after collection authorization")
        snapshot, file_sha, byte_count = evidence.regular_json(
            run_dir, saved["snapshot_path"], f"dependency snapshot {receipt_id}"
        )
        if (
            snapshot != documents[receipt_id]
            or file_sha != saved["file_sha256"]
            or byte_count != saved["bytes"]
        ):
            raise V2RunnerError("terminal dependency snapshot changed after initialization")
    manifest = terminal.derive_manifest(
        experiment.load_json(experiment.ROOT / terminal.BLOCKED_MANIFEST_TEMPLATE), documents
    )
    if manifest != load_manifest(run_dir, state):
        raise V2RunnerError("runner manifest differs from freshly derived terminal A3 authority")
    originals: list[dict[str, Any]] = []
    repeats: list[dict[str, Any]] = []
    receipt_digests: list[str] = []
    for key in sorted(state["cells"]):
        cell, receipt_digest = rederive_cell(run_dir, state, key, manifest)
        (originals if cell["campaign"] == "original" else repeats).append(cell)
        receipt_digests.append(receipt_digest)
    manifest_digest = experiment.canonical_sha256(manifest)
    receipts_digest = evidence.canonical_sha256(receipt_digests)
    policy = manifest["v2_protocol"]["product_policy"]
    result = {
        "schema": "pulp.gpu-dpr-experiment.v2",
        "version": 2,
        "status": "complete",
        "evidence_kind": "measured",
        "experiment_id": state["experiment_id"],
        "plan_revision": state["plan_revision"],
        "pulp_sha": state["pulp_sha"],
        "forge_sha": state["forge_sha"],
        "authority": {
            "manifest_sha256": manifest_digest,
            "product_policy_id": policy["id"],
            "product_policy_blob": policy["blob"],
            "a3_receipt_sha256": current_projection[terminal.PRODUCT_POLICY_ID]["sha256"],
            "a2t_receipt_sha256": current_projection[terminal.A2T_ID]["sha256"],
            "a3_runtime_receipt_sha256": current_projection[terminal.A3_ID]["sha256"],
            "runner_receipts_sha256": receipts_digest,
            "trace_analyzer_sha256": state["trace_analyzer"]["sha256"],
            "timer_noise_p95_ns": manifest["trial_contract"]["timer_noise_p95_ns"],
            "memory_sampler_resolution_bytes": manifest["trial_contract"]["memory_sampler_resolution_bytes"],
            "collection_authorized": True,
        },
        "matrix": {
            "scenario_ids": [item["id"] for item in manifest["scenarios"]],
            "modes": experiment.MODES,
            "requested_dprs": experiment.REQUESTED_DPRS,
            "cell_count": 84,
            "repeat_cell_count": 84,
        },
        "cells": originals,
        "repeat_cells": repeats,
        "analysis": experiment.compute_v2_analysis(
            originals, repeats, manifest, manifest_digest
        ),
        "publication": {
            "status": "candidate-awaiting-live-proof",
            "repository": "Generous-Corp/pulp",
            "revision": None,
            "path": terminal.CANONICAL_RESULT.as_posix(),
            "protected_main_verified": False,
            "required_checks_green": False,
        },
        "b5_gate": {},
    }
    disposition = result["analysis"]["disposition"]
    result["b5_gate"] = {
        "status": "cancelled-no-change" if disposition == "no-change" else "waiting-trigger",
        "requires": [] if disposition == "no-change" else ["B0-adopted-vellum-api-refresh"],
        "authorizes_policy_change": False,
    }
    schema_errors = experiment.json_schema_lite.validate(
        result, experiment.schema_for_lite(experiment.load_json(experiment.DEFAULT_SCHEMA))
    )
    semantic_errors = [] if schema_errors else experiment.v2_semantic_errors(
        result, manifest, manifest_digest, evidence_root=run_dir,
    )
    if schema_errors or semantic_errors:
        raise V2RunnerError("; ".join([*schema_errors, *semantic_errors]))
    result_path = run_dir / RESULT
    if result_path.exists():
        existing, _, _ = evidence.regular_json(run_dir, RESULT, "A4 v2 final result")
        if existing != result:
            raise V2RunnerError("refusing to replace a different finalized A4 v2 result")
    else:
        _write_new_json(result_path, result)
    return result


def status(run_dir: Path) -> dict[str, Any]:
    state, _ = load_state(run_dir)
    counts: dict[str, int] = {}
    for cell in state["cells"].values():
        counts[cell["status"]] = counts.get(cell["status"], 0) + 1
    return {
        "schema": "pulp.gpu-dpr-run-status.v2",
        "run_id": state["run_id"],
        "total_cells": 168,
        "complete_cells": counts.get("complete", 0),
        "status_counts": counts,
        "finalizable": counts.get("complete", 0) == 168,
    }
