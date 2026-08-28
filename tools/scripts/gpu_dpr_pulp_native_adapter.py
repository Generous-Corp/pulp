#!/usr/bin/env python3
"""Capture one real Pulp-native A4 DPR cell without fabricating missing metrics.

This adapter deliberately stops at a durable INCONCLUSIVE receipt after the
real screenshot preflight. The A4 result contract also requires correlated A2T
Perfetto evidence, the ratified A3 budget, same-process adapter identity,
frame/memory/upload samples, and logical-input/fidelity oracles. None of those
can be inferred from a PNG or subprocess wall time, so this adapter preserves
the real capture and names the producers a later measurement adapter must add.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

REQUEST_SCHEMA = "pulp.gpu-dpr-cell-request.v1"
RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-receipt.v1"
PREFLIGHT_SCHEMA = "pulp.gpu-dpr-native-preflight.v1"
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


def source_root() -> Path:
    override = os.environ.get("PULP_DPR_SOURCE_ROOT")
    return Path(override).resolve() if override else Path(__file__).resolve().parents[2]


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
    root = source_root()
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

    try:
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
