#!/usr/bin/env python3
"""Capture or measure one real Pulp-native A4 DPR cell.

With ``PULP_DPR_NATIVE_MEASUREMENT_BIN`` set, the adapter pins and invokes an
exact product measurement producer, then accepts its receipt only when capture,
metrics, logical input, trace correlation, and Dawn identity are all attested as
same-process evidence. Without that producer it deliberately stops at a durable
INCONCLUSIVE receipt after the real screenshot preflight. None of the missing
measurements can be inferred from a PNG or subprocess wall time.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import signal
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

REQUEST_SCHEMA = "pulp.gpu-dpr-cell-request.v1"
RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-receipt.v1"
PREFLIGHT_SCHEMA = "pulp.gpu-dpr-native-preflight.v1"
MEASUREMENT_SCOPE_SCHEMA = "pulp.gpu-dpr-native-measurement-scope.v1"
MEASUREMENT_ATTESTATION_SCHEMA = "pulp.gpu-dpr-native-measurement-attestation.v1"
ARTIFACT_KINDS = {"capture", "trace", "raw_samples", "input_receipt"}
SAME_PROCESS_FIELDS = {
    "adapter_identity", "capture", "frame_metrics", "memory_metrics",
    "logical_input", "trace_correlation",
}
SUPPORTED = {
    "dense-text-thin-strokes": "skia",
    "shader-heavy-controls": "gpu",
    "meters-waveforms": "gpu",
}


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measurement_binary() -> Path | None:
    value = os.environ.get("PULP_DPR_NATIVE_MEASUREMENT_BIN")
    if not value:
        return None
    path = Path(value)
    if (
        not path.is_absolute() or path.is_symlink()
        or not path.is_file() or not os.access(path, os.X_OK)
    ):
        raise ValueError(
            "PULP_DPR_NATIVE_MEASUREMENT_BIN must name an absolute, regular executable"
        )
    return path.resolve()


def checked_artifact(root: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} artifact path is missing")
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"{label} artifact path must stay inside the cell")
    path = root / relative
    resolved = path.resolve()
    if root.resolve() not in resolved.parents:
        raise ValueError(f"{label} artifact escapes the cell")
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} artifact is not a regular file")
    return path


def validate_measurement_receipt(
    request: dict[str, Any], receipt: dict[str, Any], cell_dir: Path,
    pinned_producer: Path,
) -> dict[str, Any]:
    scenario = request["scenario"]
    expected = {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "attempt_nonce": request["attempt_nonce"],
        "scenario_id": scenario["id"],
        "scenario_kind": scenario["kind"],
        "mode": request["mode"],
        "requested_dpr": request["requested_dpr"],
    }
    for field, value in expected.items():
        if receipt.get(field) != value:
            raise ValueError(f"measurement receipt {field} differs from the request")
    outcome = receipt.get("outcome")
    if outcome not in {"pass", "fail", "skip", "inconclusive"}:
        raise ValueError("measurement receipt outcome is invalid")
    if outcome in {"skip", "inconclusive"}:
        dependencies = receipt.get("dependencies")
        if (
            not isinstance(receipt.get("reason"), str) or not receipt["reason"]
            or not isinstance(dependencies, list) or not dependencies
        ):
            raise ValueError("incomplete measurement receipt lacks reason/dependencies")
        return receipt

    scope = receipt.get("measurement_scope")
    if not isinstance(scope, dict) or scope.get("schema") != MEASUREMENT_SCOPE_SCHEMA:
        raise ValueError("complete measurement receipt lacks the typed scope attestation")
    same_process = scope.get("same_process")
    if (
        not isinstance(same_process, dict)
        or set(same_process) != SAME_PROCESS_FIELDS
        or any(same_process[field] is not True for field in SAME_PROCESS_FIELDS)
    ):
        raise ValueError("measurement producer did not attest every same-process evidence field")
    if scope.get("audio_device_opened") is not False:
        raise ValueError("Pulp-native DPR measurement must not open an audio device")

    observed_dpr = effective_dpr(request)
    logical = scenario["logical_size"]
    expected_physical = {
        "width": round(float(logical["width"]) * observed_dpr),
        "height": round(float(logical["height"]) * observed_dpr),
    }
    if receipt.get("observed_dpr") != observed_dpr:
        raise ValueError("measurement receipt observed DPR differs from the requested mode")
    if receipt.get("physical_size") != expected_physical:
        raise ValueError("measurement receipt physical size differs from logical size x DPR")
    if receipt.get("content_digest") != request["expected_content_digest"]:
        raise ValueError("measurement receipt content digest differs from the frozen source")

    machine = receipt.get("machine")
    adapter = receipt.get("adapter")
    build = receipt.get("build_identity")
    if not isinstance(machine, dict) or not all(machine.get(k) for k in ("id", "os", "architecture")):
        raise ValueError("measurement receipt lacks exact machine identity")
    if (
        not isinstance(adapter, dict) or adapter.get("authentic_identity") is not True
        or adapter.get("class") != "hardware"
        or not all(adapter.get(k) for k in ("name", "backend", "driver"))
    ):
        raise ValueError("measurement receipt lacks authentic hardware adapter identity")
    if not isinstance(build, dict) or build.get("pulp_sha") != request["pulp_sha"]:
        raise ValueError("measurement receipt is not bound to the requested Pulp SHA")

    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, list):
        raise ValueError("measurement receipt artifact list is missing")
    by_kind: dict[str, Path] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise ValueError("measurement receipt contains a malformed artifact")
        kind = artifact.get("kind")
        if kind not in ARTIFACT_KINDS or kind in by_kind:
            raise ValueError("measurement artifacts must contain each canonical kind exactly once")
        path = checked_artifact(cell_dir, artifact.get("path"), str(kind))
        if artifact.get("sha256") != sha256(path):
            raise ValueError(f"{kind} artifact digest differs from its bytes")
        by_kind[str(kind)] = path
    if set(by_kind) != ARTIFACT_KINDS:
        raise ValueError("measurement receipt lacks a canonical evidence artifact")
    if physical_size(by_kind["capture"]) != (
        expected_physical["width"], expected_physical["height"]
    ):
        raise ValueError("measurement capture dimensions differ from the receipt")
    if not by_kind["trace"].read_bytes():
        raise ValueError("measurement trace artifact is empty")
    for kind in ("raw_samples", "input_receipt"):
        load_json(by_kind[kind])

    producer_digest = sha256(pinned_producer)
    build["measurement_producer"] = {
        "path": str(pinned_producer.resolve()),
        "sha256": producer_digest,
    }
    receipt["measurement_attestation"] = {
        "schema": MEASUREMENT_ATTESTATION_SCHEMA,
        "producer_sha256": producer_digest,
        "same_process": same_process,
        "audio_device_opened": False,
    }
    return receipt


def run_measurement_producer(
    request: dict[str, Any], request_path: Path, receipt_path: Path, producer: Path,
    root: Path, source: Path,
) -> int:
    cell_dir = receipt_path.parent
    nonce = request["attempt_nonce"]
    pinned = cell_dir / f"measurement-producer-{nonce}"
    if pinned.exists() or pinned.is_symlink():
        pinned.unlink()
    shutil.copyfile(producer, pinned)
    pinned.chmod(0o555)
    pinned_digest = sha256(pinned)
    producer_receipt = cell_dir / f"measurement-producer-receipt-{nonce}.json"
    if producer_receipt.exists() or producer_receipt.is_symlink():
        producer_receipt.unlink()
    process = subprocess.Popen(
        [str(pinned), "--request", str(request_path), "--receipt", str(producer_receipt)],
        cwd=cell_dir, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=os.name == "posix",
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGTERM)
            else:
                process.terminate()
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                if os.name == "posix":
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
    (cell_dir / "measurement-producer.stdout.log").write_text(
        stdout, encoding="utf-8"
    )
    (cell_dir / "measurement-producer.stderr.log").write_text(
        stderr, encoding="utf-8"
    )
    if timed_out:
        raise TimeoutError("measurement producer exceeded the 600 second limit")
    if sha256(pinned) != pinned_digest:
        raise ValueError("measurement producer changed its pinned executable during capture")
    if git_head(root) != request["pulp_sha"]:
        raise ValueError("source HEAD changed while the measurement producer was running")
    if sha256(source) != request["expected_content_digest"]:
        raise ValueError("frozen scenario source changed while the measurement producer was running")
    if not producer_receipt.is_file() or producer_receipt.is_symlink():
        raise ValueError(f"measurement producer exited {process.returncode} without a receipt")
    receipt = validate_measurement_receipt(
        request, load_json(producer_receipt), cell_dir, pinned
    )
    expected_exit = {"pass": 0, "fail": 1, "skip": 2, "inconclusive": 3}[receipt["outcome"]]
    if process.returncode != expected_exit:
        raise ValueError(
            f"measurement producer exit {process.returncode} disagrees with "
            f"receipt outcome {receipt['outcome']}"
        )
    write_json(receipt_path, receipt)
    return expected_exit


def physical_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()[:24]
    if len(data) != 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("capture is not a PNG with an IHDR")
    return struct.unpack(">II", data[16:24])


def effective_dpr(request: dict[str, Any]) -> float:
    requested = float(request["requested_dpr"])
    mode = request["mode"]
    if mode == "configured_max":
        # Frozen by test/fixtures/gpu-ux/dpr/manifest.json v1. The request
        # intentionally carries no top-level configured-max field.
        return min(requested, 2.0)
    if mode in {"exact", "adaptive_simulation"}:
        return requested
    raise ValueError(f"unsupported experiment mode: {mode}")


def source_root(request: dict[str, Any]) -> Path:
    value = request.get("pulp_source_root")
    if not isinstance(value, str) or not value:
        raise ValueError("DPR request lacks an exact Pulp source root")
    path = Path(value)
    if not path.is_absolute() or path.is_symlink() or not path.is_dir():
        raise ValueError("Pulp source root must be an absolute, non-symlink directory")
    return path.resolve()


def screenshot_binary(root: Path) -> Path:
    override = os.environ.get("PULP_DPR_SCREENSHOT_BIN")
    candidates = [
        Path(override).expanduser() if override else None,
        root / "build" / "tools" / "screenshot" / "pulp-screenshot",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise FileNotFoundError(
        "exact pulp-screenshot binary unavailable; set PULP_DPR_SCREENSHOT_BIN"
    )


def git_head(root: Path) -> str:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
    )
    return completed.stdout.strip()


def machine_id() -> dict[str, str]:
    model = "unknown"
    if sys.platform == "darwin":
        completed = subprocess.run(
            ["sysctl", "-n", "hw.model"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        if completed.returncode == 0 and completed.stdout.strip():
            model = completed.stdout.strip()
    return {
        "hostname": platform.node(),
        "model": model,
        "os": platform.platform(),
        "architecture": platform.machine(),
    }


def incomplete_receipt(
    request: dict[str, Any], reason: str, dependencies: list[str]
) -> dict[str, Any]:
    scenario = request["scenario"]
    return {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "attempt_nonce": request["attempt_nonce"],
        "scenario_id": scenario["id"],
        "scenario_kind": scenario["kind"],
        "mode": request["mode"],
        "requested_dpr": request["requested_dpr"],
        "outcome": "inconclusive",
        "reason": reason,
        "dependencies": sorted(set(dependencies)),
    }


def run(request_path: Path, receipt_path: Path) -> int:
    request = load_json(request_path)
    if request.get("schema") != REQUEST_SCHEMA or request.get("version") != 1:
        raise ValueError("unsupported DPR cell request")
    scenario = request.get("scenario")
    if not isinstance(scenario, dict) or scenario.get("id") not in SUPPORTED:
        scenario_id = scenario.get("id", "unknown") if isinstance(scenario, dict) else "unknown"
        receipt = incomplete_receipt(
            request, f"Pulp-native screenshot adapter does not own {scenario_id}",
            [f"adapter:{scenario_id}"],
        )
        write_json(receipt_path, receipt)
        return 3

    scenario_id = scenario["id"]
    dependencies = [
        "a2t:correlated-cell-trace",
        "a3:ratified-budget-receipt",
        f"dpr-metrics:{scenario_id}",
        f"logical-input-oracle:{scenario_id}",
        f"reference-fidelity-oracle:{scenario_id}",
    ]
    if SUPPORTED[scenario_id] == "gpu":
        dependencies.append(f"same-process-adapter-identity:{scenario_id}")
    if "small_text" in scenario.get("required_oracles", []):
        dependencies.append(f"small-text-legibility-oracle:{scenario_id}")
    if "thin_strokes" in scenario.get("required_oracles", []):
        dependencies.append(f"thin-stroke-oracle:{scenario_id}")

    producer_requested = bool(os.environ.get("PULP_DPR_NATIVE_MEASUREMENT_BIN"))
    try:
        root = source_root(request)
        head = git_head(root)
        if head != request["pulp_sha"]:
            raise ValueError(
                f"source HEAD {head} differs from requested Pulp SHA {request['pulp_sha']}"
            )
        source = root / "test" / "fixtures" / "gpu-ux" / "dpr" / scenario["source"]
        if not source.is_file():
            raise FileNotFoundError(f"frozen scenario source missing: {source}")
        actual_source_digest = sha256(source)
        if actual_source_digest != request["expected_content_digest"]:
            raise ValueError("frozen scenario source digest differs from the request")
        producer = measurement_binary()
        if producer is not None:
            return run_measurement_producer(
                request, request_path, receipt_path, producer, root, source
            )
        executable = screenshot_binary(root)
        observed_dpr = effective_dpr(request)
        logical = scenario["logical_size"]
        capture = receipt_path.parent / "capture.png"
        command = [
            str(executable), "--script", str(source), "--output", str(capture),
            "--width", str(logical["width"]), "--height", str(logical["height"]),
            "--scale", f"{observed_dpr:g}", "--backend", SUPPORTED[scenario_id],
        ]
        started = time.monotonic_ns()
        completed = subprocess.run(
            command, cwd=receipt_path.parent, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180,
        )
        elapsed_ms = (time.monotonic_ns() - started) / 1_000_000.0
        (receipt_path.parent / "capture.stdout.log").write_text(
            completed.stdout, encoding="utf-8"
        )
        (receipt_path.parent / "capture.stderr.log").write_text(
            completed.stderr, encoding="utf-8"
        )
        if completed.returncode != 0:
            raise RuntimeError(f"pulp-screenshot exited {completed.returncode}")
        width, height = physical_size(capture)
        expected_width = round(float(logical["width"]) * observed_dpr)
        expected_height = round(float(logical["height"]) * observed_dpr)
        if (width, height) != (expected_width, expected_height):
            raise ValueError(
                f"capture was {width}x{height}; expected {expected_width}x{expected_height}"
            )
        preflight = {
            "schema": PREFLIGHT_SCHEMA,
            "version": 1,
            "status": "capture-complete-measurement-incomplete",
            "cell_key": request["cell_key"],
            "scenario_id": scenario_id,
            "mode": request["mode"],
            "requested_dpr": request["requested_dpr"],
            "observed_dpr": observed_dpr,
            "logical_size": logical,
            "physical_size": {"width": width, "height": height},
            "source": str(source),
            "source_sha256": actual_source_digest,
            "capture": "capture.png",
            "capture_sha256": sha256(capture),
            "capture_bytes": capture.stat().st_size,
            "capture_process_wall_ms": elapsed_ms,
            "capture_backend_requested": SUPPORTED[scenario_id],
            "capture_backend_report": completed.stdout.strip(),
            "binary": {"path": str(executable), "sha256": sha256(executable)},
            "pulp_sha": head,
            "machine": machine_id(),
            "not_claimed": [
                "cpu_frame_time", "gpu_frame_time", "first_frame_time",
                "interaction_latency", "resident_bytes", "upload_bytes",
                "capture_similarity", "small_text_legible",
                "thin_strokes_preserved", "logical_input_correct",
                "authentic_same_process_adapter", "correlated_perfetto_trace",
            ],
        }
        write_json(receipt_path.parent / "preflight.json", preflight)
        reason = (
            "real Pulp capture and physical DPR preflight passed, but the cell lacks "
            "the correlated trace, ratified budget, frame/memory/upload samples, "
            "and independent fidelity/input oracles required for measured A4 evidence"
        )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        if producer_requested:
            dependencies.append(f"native-measurement-producer:{scenario_id}")
            reason = f"real Pulp same-process measurement could not complete: {error}"
        else:
            dependencies.append(f"native-capture:{scenario_id}")
            reason = f"real Pulp capture preflight could not complete: {error}"

    write_json(receipt_path, incomplete_receipt(request, reason, dependencies))
    return 3


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    try:
        return run(args.request.resolve(), args.receipt.resolve())
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"gpu DPR native adapter error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
