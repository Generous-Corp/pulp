#!/usr/bin/env python3
"""Run one authentic browser/WebGL2 A4 DPR cell.

The adapter pins an executable measurement script, binds an exact Chrome or
Chromium executable, and accepts terminal evidence only when the script returns
the complete browser attestation and canonical artifacts. Missing build/browser
prerequisites remain a durable inconclusive result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

REQUEST_SCHEMA = "pulp.gpu-dpr-cell-request.v1"
RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-receipt.v1"
SCOPE_SCHEMA = "pulp.gpu-dpr-browser-measurement-scope.v1"
ATTESTATION_SCHEMA = "pulp.gpu-dpr-browser-measurement-attestation.v1"
FIRST_FRAME_SCHEMA = "pulp.gpu-dpr-first-frame-trial.v1"
ARTIFACT_KINDS = {"capture", "trace", "raw_samples", "input_receipt"}
SAME_PROCESS_FIELDS = {
    "adapter_identity", "capture", "frame_metrics", "memory_metrics",
    "logical_input", "trace_correlation",
}
OUTPUT_CAP_BYTES = 1024 * 1024
WEB_UI_NAMES = (
    "PulpSuperConvolverUi.data", "PulpSuperConvolverUi.js",
    "PulpSuperConvolverUi.wasm",
)


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def web_ui_identity(build: Path) -> tuple[dict[str, dict[str, str]], str]:
    identities: dict[str, dict[str, str]] = {}
    binding = ""
    for name in WEB_UI_NAMES:
        path = build / name
        if path.is_symlink() or not path.is_file() or path.resolve().parent != build.resolve():
            raise FileNotFoundError(f"exact web UI artifact is unavailable: {name}")
        digest = sha256(path)
        identities[name] = {"path": str(path.resolve()), "sha256": digest}
        binding += f"{name}:{digest}\n"
    return identities, hashlib.sha256(binding.encode()).hexdigest()


def exact_executable(value: str | None, label: str) -> Path:
    if not value:
        raise FileNotFoundError(f"{label} is unavailable")
    path = Path(value)
    if (
        not path.is_absolute() or path.is_symlink() or not path.is_file()
        or not os.access(path, os.X_OK)
    ):
        raise ValueError(f"{label} must be an absolute regular executable")
    return path.resolve()


def browser_product_identity(path: Path) -> dict[str, str]:
    completed = subprocess.run(
        [str(path), "--version"], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=15,
    )
    version = completed.stdout.strip()
    if completed.returncode != 0 or not any(
        name in version for name in ("Google Chrome", "Chromium")
    ):
        raise ValueError("browser executable does not identify as Chrome/Chromium")
    identity = {
        "version": version,
        "codesign_identifier": "not-applicable",
        "team_identifier": "not-applicable",
    }
    if sys.platform == "darwin":
        verify = subprocess.run(
            ["/usr/bin/codesign", "--verify", "--strict", str(path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
        )
        details = subprocess.run(
            ["/usr/bin/codesign", "-dv", "--verbose=4", str(path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
        )
        identifiers = ("Identifier=com.google.Chrome", "Identifier=org.chromium.Chromium")
        if verify.returncode != 0 or details.returncode != 0 or not any(
            item in details.stdout for item in identifiers
        ):
            raise ValueError("browser executable lacks an accepted Chrome/Chromium signature")
        identity["codesign_identifier"] = next(
            (line.split("=", 1)[1] for line in details.stdout.splitlines()
             if line.startswith("Identifier=")), "",
        )
        identity["team_identifier"] = next(
            (line.split("=", 1)[1] for line in details.stdout.splitlines()
             if line.startswith("TeamIdentifier=")), "",
        )
        if (
            identity["team_identifier"] in {"", "not set"}
            or identity["codesign_identifier"] == "com.google.Chrome"
            and identity["team_identifier"] != "EQHXZ8M8AV"
        ):
            raise ValueError("browser executable lacks an accepted publisher identity")
    return identity


def effective_dpr(request: dict[str, Any]) -> float:
    requested = float(request["requested_dpr"])
    if request["mode"] == "configured_max":
        return min(requested, 2.0)
    if request["mode"] in {"exact", "adaptive_simulation"}:
        return requested
    raise ValueError("unsupported DPR mode")


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()[:24]
    if len(data) != 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("browser capture is not a PNG")
    return struct.unpack(">II", data[16:24])


def checked_artifact(root: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} artifact path is missing")
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"{label} artifact escapes the cell")
    path = root / relative
    if path.is_symlink() or not path.is_file() or root.resolve() not in path.resolve().parents:
        raise ValueError(f"{label} artifact is not a confined regular file")
    return path


def incomplete(request: dict[str, Any], reason: str) -> dict[str, Any]:
    scenario = request.get("scenario", {})
    return {
        "schema": RECEIPT_SCHEMA, "version": 1,
        "attempt_nonce": request.get("attempt_nonce"),
        "scenario_id": scenario.get("id", "unknown"),
        "scenario_kind": scenario.get("kind", "unknown"),
        "mode": request.get("mode"), "requested_dpr": request.get("requested_dpr"),
        "outcome": "inconclusive", "reason": reason,
        "dependencies": ["browser:authentic-webgl2-measurement"],
    }


def validate_receipt(
    request: dict[str, Any], receipt: dict[str, Any], cell: Path,
    script: Path, browser: Path, build_artifacts: dict[str, dict[str, str]],
    build_digest: str,
) -> dict[str, Any]:
    scenario = request["scenario"]
    expected = {
        "schema": RECEIPT_SCHEMA, "version": 1,
        "attempt_nonce": request["attempt_nonce"],
        "attempt_number": request["attempt_number"],
        "scenario_id": "super-convolver-web",
        "scenario_kind": "maintained_web_canary",
        "mode": request["mode"], "requested_dpr": request["requested_dpr"],
    }
    for field, value in expected.items():
        if receipt.get(field) != value:
            raise ValueError(f"browser receipt {field} differs from request")
    if receipt.get("outcome") not in {"pass", "fail"}:
        raise ValueError("browser producer did not return a terminal outcome")
    dpr = effective_dpr(request)
    logical = scenario["logical_size"]
    physical = {
        "width": round(logical["width"] * dpr),
        "height": round(logical["height"] * dpr),
    }
    if receipt.get("observed_dpr") != dpr or receipt.get("physical_size") != physical:
        raise ValueError("browser receipt does not prove the requested physical DPR")
    if receipt.get("content_digest") != request["expected_content_digest"]:
        raise ValueError("browser receipt content digest differs from request")
    adapter = receipt.get("adapter")
    identity = " ".join(str(adapter.get(k, "")) for k in ("name", "driver")) if isinstance(adapter, dict) else ""
    if (
        not isinstance(adapter, dict) or adapter.get("class") != "hardware"
        or adapter.get("api") != "webgl2" or adapter.get("authentic_identity") is not True
        or not all(adapter.get(k) for k in ("name", "backend", "driver"))
        or any(word in identity.lower() for word in ("swiftshader", "software", "llvmpipe", "lavapipe"))
    ):
        raise ValueError("browser producer did not prove authentic hardware WebGL2")
    scope = receipt.get("measurement_scope")
    same = scope.get("same_process") if isinstance(scope, dict) else None
    if (
        not isinstance(scope, dict) or scope.get("schema") != SCOPE_SCHEMA
        or not isinstance(same, dict) or set(same) != SAME_PROCESS_FIELDS
        or any(same[k] is not True for k in SAME_PROCESS_FIELDS)
        or scope.get("audio_device_opened") is not False
    ):
        raise ValueError("browser producer lacks complete same-process scope")
    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, list):
        raise ValueError("browser receipt lacks artifacts")
    by_kind: dict[str, Path] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise ValueError("malformed browser artifact")
        kind = artifact.get("kind")
        if kind not in ARTIFACT_KINDS or kind in by_kind:
            raise ValueError("browser artifact kinds must be canonical and unique")
        path = checked_artifact(cell, artifact.get("path"), str(kind))
        if sha256(path) != artifact.get("sha256"):
            raise ValueError(f"{kind} artifact digest differs from bytes")
        by_kind[str(kind)] = path
    if set(by_kind) != ARTIFACT_KINDS:
        raise ValueError("browser receipt lacks a canonical artifact")
    if png_size(by_kind["capture"]) != (physical["width"], physical["height"]):
        raise ValueError("browser capture dimensions differ from logical DPR")
    raw = load_json(by_kind["raw_samples"])
    metrics = raw.get("metrics")
    gpu = metrics.get("gpu_frame_time") if isinstance(metrics, dict) else None
    if (
        not isinstance(gpu, list) or len(gpu) < 30
        or any(isinstance(v, bool) or not isinstance(v, (int, float))
               or not math.isfinite(float(v)) or float(v) <= 0 for v in gpu)
    ):
        raise ValueError("browser GPU timing is missing, zero, or synthetic")
    trials = raw.get("fresh_process_trials")
    if not isinstance(trials, list) or len(trials) != 20:
        raise ValueError("browser evidence lacks 20 fresh process trials")
    browser_digest = sha256(browser)
    pids: set[int] = set()
    for trial in trials:
        pid = trial.get("pid") if isinstance(trial, dict) else None
        if (
            not isinstance(trial, dict) or trial.get("schema") != FIRST_FRAME_SCHEMA
            or trial.get("attempt_nonce") != request["attempt_nonce"]
            or trial.get("attempt_number") != request["attempt_number"]
            or trial.get("producer_sha256") != browser_digest
            or trial.get("content_digest") != request["expected_content_digest"]
            or trial.get("pulp_sha") != request["pulp_sha"]
            or trial.get("build_sha256") != build_digest
            or trial.get("adapter") != adapter
            or isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0 or pid in pids
        ):
            raise ValueError("fresh browser process ledger is mixed or unbound")
        pids.add(pid)
    build = receipt.get("build_identity")
    if (
        not isinstance(build, dict) or build.get("pulp_sha") != request["pulp_sha"]
        or build.get("web_ui_artifacts") != build_artifacts
        or build.get("web_ui_bundle_sha256") != build_digest
    ):
        raise ValueError("browser receipt lacks exact Pulp identity")
    script_digest = sha256(script)
    build["measurement_producer"] = {"path": str(browser), "sha256": browser_digest}
    build["browser_product_executable"] = {
        "path": str(browser), "sha256": browser_digest,
    }
    build["measurement_script"] = {"path": str(script), "sha256": script_digest}
    receipt["measurement_attestation"] = {
        "schema": ATTESTATION_SCHEMA,
        "producer_sha256": browser_digest,
        "script_sha256": script_digest,
        "build_sha256": build_digest,
        "same_process": same,
        "audio_device_opened": False,
    }
    return receipt


def communicate_bounded(process: subprocess.Popen[bytes]) -> tuple[bytes, bytes]:
    buffers = [bytearray(), bytearray()]
    exceeded = threading.Event()
    def drain(index: int, stream: Any) -> None:
        while chunk := stream.read(65536):
            remaining = OUTPUT_CAP_BYTES - len(buffers[index])
            buffers[index].extend(chunk[:max(0, remaining)])
            if len(chunk) > remaining:
                exceeded.set(); return
    threads = [
        threading.Thread(target=drain, args=(0, process.stdout), daemon=True),
        threading.Thread(target=drain, args=(1, process.stderr), daemon=True),
    ]
    for thread in threads: thread.start()
    deadline = time.monotonic() + 900
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or exceeded.wait(max(0.0, min(0.05, remaining))):
            try:
                os.killpg(process.pid, signal.SIGTERM) if os.name == "posix" else process.terminate()
            except ProcessLookupError:
                pass
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try: os.killpg(process.pid, signal.SIGKILL) if os.name == "posix" else process.kill()
                except ProcessLookupError: pass
                process.wait()
            if remaining <= 0: raise TimeoutError("browser producer exceeded 900 seconds")
            raise ValueError(f"browser producer output exceeded {OUTPUT_CAP_BYTES} bytes per stream")
    for thread in threads: thread.join(timeout=2)
    return bytes(buffers[0]), bytes(buffers[1])


def run(request_path: Path, receipt_path: Path) -> int:
    request = load_json(request_path)
    scenario = request.get("scenario")
    if request.get("schema") != REQUEST_SCHEMA or request.get("version") != 1:
        raise ValueError("unsupported DPR request")
    if not isinstance(scenario, dict) or scenario.get("id") != "super-convolver-web" or scenario.get("kind") != "maintained_web_canary":
        write_json(receipt_path, incomplete(request, "browser adapter owns only super-convolver-web"))
        return 3
    root = Path(request.get("pulp_source_root", ""))
    if not root.is_absolute() or root.is_symlink() or not root.is_dir():
        write_json(receipt_path, incomplete(request, "Pulp source root is not exact")); return 3
    root = root.resolve()
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    source = root / scenario["source"]
    try:
        if head != request["pulp_sha"] or not source.is_file() or sha256(source) != request["expected_content_digest"]:
            raise ValueError("Pulp head or maintained web source differs from request")
        producer = exact_executable(os.environ.get("PULP_DPR_WEB_MEASUREMENT_BIN"), "browser measurement script")
        browser = exact_executable(os.environ.get("PULP_DPR_BROWSER_BIN"), "Chrome/Chromium")
        browser_identity = browser_product_identity(browser)
        build = Path(os.environ.get("PULP_DPR_WEB_BUILD_DIR", ""))
        if not build.is_absolute() or build.is_symlink():
            raise FileNotFoundError("exact SuperConvolver web UI build is unavailable")
        build = build.resolve()
        build_artifacts, build_digest = web_ui_identity(build)
        cell = receipt_path.parent
        pinned = cell / f"browser-measurement-{request['attempt_nonce']}{producer.suffix}"
        shutil.copyfile(producer, pinned); pinned.chmod(0o555)
        produced = cell / f"browser-receipt-{request['attempt_nonce']}.json"
        process = subprocess.Popen(
            [str(pinned), "--request", str(request_path), "--receipt", str(produced),
             "--browser", str(browser), "--build", str(build.resolve())],
            cwd=cell, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=os.name == "posix",
        )
        stdout, stderr = communicate_bounded(process)
        (cell / "browser.stdout.log").write_bytes(stdout)
        (cell / "browser.stderr.log").write_bytes(stderr)
        if not produced.is_file() or produced.is_symlink():
            raise ValueError(f"browser producer exited {process.returncode} without receipt")
        receipt = validate_receipt(
            request, load_json(produced), cell, pinned, browser,
            build_artifacts, build_digest,
        )
        receipt["build_identity"]["browser_product"] = browser_identity
        expected_exit = 0 if receipt["outcome"] == "pass" else 1
        if process.returncode != expected_exit:
            raise ValueError("browser producer exit disagrees with receipt")
        if (
            sha256(source) != request["expected_content_digest"]
            or sha256(pinned) != sha256(producer)
            or web_ui_identity(build) != (build_artifacts, build_digest)
        ):
            raise ValueError("browser source or pinned measurement script changed during run")
        write_json(receipt_path, receipt)
        return expected_exit
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        write_json(receipt_path, incomplete(request, f"real browser measurement could not complete: {error}"))
        return 3


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    try:
        return run(args.request.resolve(), args.receipt.resolve())
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"gpu DPR web adapter error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
