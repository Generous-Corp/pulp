#!/usr/bin/env python3
"""Run one fail-closed external A3 first-visible product campaign.

The runner owns evidence confinement, exact adapter/budget snapshots, timeout
handling, and validation. A role-specific adapter owns the real product or DAW
lifecycle and must report the cache boundary it actually exercised; the runner
never relabels a trial as cold or warm from timing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import secrets
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_first_visible_a3_acceptance as a3  # noqa: E402

REQUEST_SCHEMA = "pulp.gpu-first-visible-campaign-request.v1"
ADAPTER_SCHEMA = "pulp.gpu-first-visible-campaign-adapter.v1"
RUN_SCHEMA = "pulp.gpu-first-visible-campaign-run.v1"
OUTPUT_CAP_BYTES = 1024 * 1024
OUTCOME_EXIT = {"pass": 0, "fail": 1, "inconclusive": 2, "skip": 3}
ADAPTER_ARTIFACT_KEYS = {
    "health_result", "raw_cold", "raw_warm", "product_artifact",
    "host_artifact", "trace", "trace_analysis", "blank_negative",
    "audio_thread_exclusion", "measurement_producer",
    "blank_control_binary", "audio_control_binary",
}


class CampaignError(ValueError):
    """An adapter result cannot support the requested A3 campaign role."""


def regular_file_bytes(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise CampaignError(f"{label} is not a readable regular file: {path}") from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise CampaignError(f"{label} is not a regular file: {path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def regular_json(path: Path, label: str) -> dict[str, Any]:
    try:
        payload = json.loads(regular_file_bytes(path, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(f"{label} is not valid JSON: {path}") from error
    if not isinstance(payload, dict):
        raise CampaignError(f"{label} must contain a JSON object")
    return payload


def atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False,
    ) as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    os.replace(temporary, path)


def snapshot_file(
    source: Path, destination: Path, label: str, *, executable: bool = False,
) -> dict[str, str]:
    data = regular_file_bytes(source, label)
    digest = hashlib.sha256(data).hexdigest()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise CampaignError(f"runner snapshot already exists: {destination}")
    with tempfile.NamedTemporaryFile("wb", dir=destination.parent, delete=False) as handle:
        handle.write(data)
        temporary = Path(handle.name)
    temporary.chmod(0o555 if executable else 0o444)
    os.replace(temporary, destination)
    return {"path": destination.resolve().as_posix(), "sha256": digest}


def relative_ref(ref: dict[str, str], run_dir: Path) -> dict[str, str]:
    return {
        "path": Path(ref["path"]).resolve().relative_to(run_dir.resolve()).as_posix(),
        "sha256": ref["sha256"],
    }


def validate_identity(identity: Any, role: str) -> dict[str, Any]:
    a3.exact_keys(identity, a3.IDENTITY_KEYS, "campaign identity")
    assert isinstance(identity, dict)
    for field in (
        "build_id", "product_id", "product_name", "machine_id", "instance_id",
        "campaign_id",
    ):
        value = identity[field]
        if not isinstance(value, str) or not value or len(value) > 256:
            raise CampaignError(f"campaign identity.{field} must be bounded and nonempty")
    if (
        not isinstance(identity["pulp_revision"], str)
        or not re.fullmatch(r"[0-9a-f]{40}", identity["pulp_revision"])
    ):
        raise CampaignError("campaign identity requires an exact Pulp revision")
    forge_revision = identity["forge_revision"]
    if forge_revision is not None and (
        not isinstance(forge_revision, str)
        or not re.fullmatch(r"[0-9a-f]{40}", forge_revision)
    ):
        raise CampaignError("campaign identity Forge revision is invalid")
    expected_format = {
        "standalone": {"standalone"},
        "headless-constrained": {"headless"},
        "daw": {"auv2", "vst3", "clap"},
        "forge": {"standalone"},
    }[role]
    if identity["plugin_format"] not in expected_format:
        raise CampaignError(f"{role} campaign has the wrong product/plugin format")
    if role == "forge" and forge_revision is None:
        raise CampaignError("Forge campaign requires an exact Forge revision")
    return identity


def snapshot_budget(
    run_dir: Path, identity: dict[str, Any], receipt: Path, cold: Path, warm: Path,
) -> tuple[dict[str, Any], dict[str, dict[str, str]]]:
    sources = {"receipt": receipt, "raw_cold": cold, "raw_warm": warm}
    refs: dict[str, dict[str, str]] = {}
    for name, source in sources.items():
        suffix = source.suffix if source.suffix else ".bin"
        captured = snapshot_file(source, run_dir / "budget" / f"{name}{suffix}", f"budget {name}")
        refs[name] = relative_ref(captured, run_dir)
    ratification = a3.artifact_json(refs["receipt"], run_dir, "budget.receipt")
    plan_revision = ratification.get("plan_revision")
    pulp_revision = ratification.get("pulp_revision")
    if not isinstance(plan_revision, str) or not re.fullmatch(r"[0-9a-f]{40}", plan_revision):
        raise CampaignError("ratified budget lacks an exact planning revision")
    if pulp_revision != identity["pulp_revision"]:
        raise CampaignError("ratified budget Pulp revision differs from campaign identity")
    stub = {
        "plan": {"revision": plan_revision},
        "identity": {"pulp_revision": pulp_revision},
        "budget": {
            "id": "pulp.editor-first-visible.v1",
            "version": 1,
            "status": "ratified",
            **refs,
        },
    }
    try:
        validated = a3.validate_budget(stub, run_dir)
    except a3.AcceptanceError as error:
        raise CampaignError(str(error)) from error
    return validated, refs


def terminate_adapter(process: subprocess.Popen[bytes]) -> None:
    try:
        if os.name == "posix":
            os.killpg(process.pid, 15)
        else:
            process.terminate()
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(process.pid, 9)
        else:
            process.kill()
        process.wait()


def communicate_bounded(
    process: subprocess.Popen[bytes], timeout_seconds: float,
) -> tuple[str, str, bool, bool]:
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

    assert process.stdout is not None and process.stderr is not None
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
            terminate_adapter(process)
            break
        if exceeded.wait(min(0.05, remaining)):
            terminate_adapter(process)
            break
    for thread in threads:
        thread.join(timeout=2)
    return (
        buffers[0].decode("utf-8", errors="replace"),
        buffers[1].decode("utf-8", errors="replace"),
        timed_out, exceeded.is_set(),
    )


def validate_adapter_ref(
    ref: Any, run_dir: Path, label: str, *, required: bool,
) -> dict[str, str] | None:
    if ref is None:
        if required:
            raise CampaignError(f"passing adapter omitted {label}")
        return None
    a3.exact_keys(ref, a3.ARTIFACT_KEYS, label)
    assert isinstance(ref, dict)
    relative = Path(ref["path"])
    if relative.parts[:2] != ("adapter-output", "artifacts"):
        raise CampaignError(f"{label} must live in the runner-owned adapter artifact directory")
    try:
        a3.resolve_artifact(ref, run_dir, label)
    except a3.AcceptanceError as error:
        raise CampaignError(str(error)) from error
    return ref


def validate_adapter_receipt(
    receipt: dict[str, Any], *, request: dict[str, Any], run_dir: Path,
    budget_receipt: dict[str, Any], adapter_exit: int,
) -> tuple[str, dict[str, Any] | None, dict[str, Any]]:
    a3.exact_keys(receipt, {
        "schema", "version", "attempt_nonce", "role", "outcome", "reason",
        "dependencies", "identity", "measurement_endpoint", "artifacts",
    }, "adapter receipt")
    if receipt["schema"] != ADAPTER_SCHEMA or receipt["version"] != 1:
        raise CampaignError("adapter receipt has the wrong schema or version")
    if receipt["attempt_nonce"] != request["attempt_nonce"]:
        raise CampaignError("adapter receipt attempt nonce differs from the issued request")
    if receipt["role"] != request["role"]:
        raise CampaignError("adapter receipt role differs from the issued request")
    outcome = receipt["outcome"]
    if outcome not in OUTCOME_EXIT or adapter_exit != OUTCOME_EXIT[outcome]:
        raise CampaignError("adapter exit code disagrees with its declared outcome")
    identity = validate_identity(receipt["identity"], receipt["role"])
    if identity != request["identity"]:
        raise CampaignError("adapter receipt identity differs from the issued request")
    endpoint = a3.MEASUREMENT_ENDPOINT_BY_ROLE[receipt["role"]]
    if receipt["measurement_endpoint"] != endpoint:
        raise CampaignError("adapter receipt has the wrong role measurement endpoint")
    dependencies = receipt["dependencies"]
    if (
        not isinstance(dependencies, list)
        or len(dependencies) > 32
        or len(set(dependencies)) != len(dependencies)
        or any(not isinstance(item, str) or not item or len(item) > 256 for item in dependencies)
    ):
        raise CampaignError("adapter receipt dependencies are invalid")
    reason = receipt["reason"]
    if outcome == "pass":
        if reason is not None or dependencies:
            raise CampaignError("passing adapter receipt cannot carry blockers")
    elif not isinstance(reason, str) or not reason or len(reason) > 1024:
        raise CampaignError("non-passing adapter receipt requires a bounded reason")

    artifacts = receipt["artifacts"]
    a3.exact_keys(artifacts, ADAPTER_ARTIFACT_KEYS, "adapter receipt artifacts")
    core_artifacts = {
        "health_result", "raw_cold", "raw_warm", "product_artifact",
        "host_artifact", "trace", "trace_analysis", "measurement_producer",
    }
    control_artifacts = {
        "blank_negative", "audio_thread_exclusion", "blank_control_binary",
        "audio_control_binary",
    }
    resolved = {
        name: validate_adapter_ref(
            value, run_dir, f"adapter.{name}",
            required=(
                outcome == "pass"
                and (
                    name in core_artifacts
                    or (request["require_controls"] and name in control_artifacts)
                )
            ),
        )
        for name, value in artifacts.items()
    }
    if outcome != "pass":
        return outcome, None, resolved
    require_controls = request["require_controls"]
    for name in control_artifacts:
        if require_controls and resolved[name] is None:
            raise CampaignError(f"requested control artifact is missing: {name}")
        if not require_controls and resolved[name] is not None:
            raise CampaignError(f"unrequested control artifact was supplied: {name}")

    assert all(resolved[name] is not None for name in (
        "health_result", "raw_cold", "raw_warm", "product_artifact",
        "host_artifact", "trace", "trace_analysis", "measurement_producer",
    ))
    campaign = {
        "role": request["role"],
        "measurement_endpoint": endpoint,
        "status": "pass",
        "identity": identity,
        **{name: resolved[name] for name in (
            "health_result", "raw_cold", "raw_warm", "product_artifact",
            "host_artifact", "trace", "trace_analysis",
        )},
    }
    try:
        cold_payload = a3.artifact_json(campaign["raw_cold"], run_dir, "campaign.raw_cold")
        warm_payload = a3.artifact_json(campaign["raw_warm"], run_dir, "campaign.raw_warm")
        cold = a3.validate_raw_samples(
            cold_payload, schema="pulp.gpu-first-visible-campaign-raw.v1",
            cache_state="cold", label="campaign.raw_cold", identity=identity,
        )
        warm = a3.validate_raw_samples(
            warm_payload, schema="pulp.gpu-first-visible-campaign-raw.v1",
            cache_state="warm", label="campaign.raw_warm", identity=identity,
        )
        health = a3.artifact_json(campaign["health_result"], run_dir, "campaign.health")
        a3.validate_health(
            health, identity=identity, budget_receipt=budget_receipt,
            cold_samples=cold, warm_samples=warm, role=request["role"],
            measurement_endpoint=endpoint, label="campaign.health",
        )
        if not a3.resolve_artifact(
            campaign["product_artifact"], run_dir, "campaign.product"
        ).data:
            raise CampaignError("campaign product artifact is empty")
        if not a3.resolve_artifact(campaign["host_artifact"], run_dir, "campaign.host").data:
            raise CampaignError("campaign host artifact is empty")
        a3.validate_campaign_trace(campaign, health, run_dir, "campaign")
        if require_controls:
            control_receipt = {
                "blank_negative": {
                    "status": "caught", "diagnostic_code": "gpu.startup.blank",
                    "receipt": resolved["blank_negative"],
                },
                "audio_thread_exclusion": {
                    "status": "pass", "receipt": resolved["audio_thread_exclusion"],
                },
            }
            a3.validate_controls(control_receipt, run_dir)
    except a3.AcceptanceError as error:
        raise CampaignError(str(error)) from error
    return outcome, campaign, resolved


def write_logs(run_dir: Path, stdout: str, stderr: str) -> dict[str, dict[str, str]]:
    refs: dict[str, dict[str, str]] = {}
    for name, value in (("stdout", stdout), ("stderr", stderr)):
        path = run_dir / "logs" / f"adapter.{name}.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding="utf-8")
        refs[name] = {
            "path": path.relative_to(run_dir).as_posix(),
            "sha256": hashlib.sha256(value.encode("utf-8")).hexdigest(),
        }
    return refs


def run_role(args: argparse.Namespace) -> int:
    if args.role not in a3.CAMPAIGN_ROLES:
        raise CampaignError(f"unknown campaign role: {args.role}")
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        raise CampaignError("adapter timeout must be positive and finite")
    run_dir = args.output_dir
    if run_dir.exists() or run_dir.is_symlink():
        raise CampaignError("campaign output directory must not already exist")
    run_dir.mkdir(parents=True)

    identity = validate_identity(regular_json(args.identity, "campaign identity"), args.role)
    identity_snapshot = snapshot_file(
        args.identity, run_dir / "inputs" / "identity.json", "campaign identity"
    )
    identity_ref = relative_ref(identity_snapshot, run_dir)
    budget_receipt, budget_refs = snapshot_budget(
        run_dir, identity, args.budget_receipt, args.budget_cold, args.budget_warm,
    )
    adapter_suffix = args.adapter.suffix
    adapter_snapshot = snapshot_file(
        args.adapter, run_dir / "tooling" / f"adapter{adapter_suffix}",
        "campaign adapter", executable=True,
    )
    adapter_ref = relative_ref(adapter_snapshot, run_dir)
    attempt_nonce = secrets.token_hex(16)
    artifact_dir = run_dir / "adapter-output" / "artifacts"
    artifact_dir.mkdir(parents=True)
    request = {
        "schema": REQUEST_SCHEMA,
        "version": 1,
        "attempt_nonce": attempt_nonce,
        "role": args.role,
        "identity": identity,
        "measurement_endpoint": a3.MEASUREMENT_ENDPOINT_BY_ROLE[args.role],
        "cold_trial_count": 10,
        "warm_trial_count": 10,
        "cold_cache_provenance": ["fresh-process", "explicit-cache-reset"],
        "warm_cache_provenance": ["same-process-editor-reopen"],
        "require_controls": args.require_controls,
        "budget": budget_refs,
        "artifact_directory": str(artifact_dir.resolve()),
    }
    request_path = run_dir / "request.json"
    atomic_json(request_path, request)
    receipt_path = run_dir / "adapter-output" / "receipt.json"
    process = subprocess.Popen(
        [adapter_snapshot["path"], "--request", str(request_path.resolve()),
         "--receipt", str(receipt_path.resolve())],
        cwd=run_dir, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, start_new_session=True,
    )
    stdout, stderr, timed_out, output_exceeded = communicate_bounded(
        process, args.timeout_seconds
    )
    logs = write_logs(run_dir, stdout, stderr)
    base = {
        "schema": RUN_SCHEMA,
        "version": 1,
        "attempt_nonce": attempt_nonce,
        "role": args.role,
        "identity": identity,
        "identity_source": identity_ref,
        "measurement_endpoint": request["measurement_endpoint"],
        "adapter": adapter_ref,
        "budget": budget_refs,
        "logs": logs,
    }
    output_path = run_dir / "run.json"
    try:
        a3.validate_declared_artifacts({
            "identity_source": identity_ref,
            "adapter": adapter_ref,
            "budget": budget_refs,
        }, run_dir, "runner-owned inputs")
    except a3.AcceptanceError as error:
        atomic_json(output_path, {
            **base, "status": "fail", "reason": str(error),
            "dependencies": ["adapter:mutated-runner-input"],
            "campaign": None, "controls": None,
        })
        return 1
    if timed_out or output_exceeded:
        reason = (
            f"adapter output exceeded {OUTPUT_CAP_BYTES} bytes per stream"
            if output_exceeded else
            f"adapter exceeded the {args.timeout_seconds:g}s bounded runtime"
        )
        atomic_json(output_path, {
            **base, "status": "incomplete", "reason": reason,
            "dependencies": ["adapter:output-limit" if output_exceeded else "adapter:timeout"],
            "campaign": None, "controls": None,
        })
        return 2
    if not receipt_path.is_file():
        atomic_json(output_path, {
            **base, "status": "incomplete",
            "reason": "adapter did not produce its exact attempt receipt",
            "dependencies": ["adapter:receipt"], "campaign": None, "controls": None,
        })
        return 2
    try:
        receipt = regular_json(receipt_path, "adapter receipt")
        outcome, campaign, artifacts = validate_adapter_receipt(
            receipt, request=request, run_dir=run_dir,
            budget_receipt=budget_receipt, adapter_exit=process.returncode,
        )
    except (CampaignError, a3.AcceptanceError) as error:
        atomic_json(output_path, {
            **base, "status": "fail", "reason": str(error),
            "dependencies": ["adapter:invalid-evidence"],
            "campaign": None, "controls": None,
        })
        return 1
    if outcome != "pass":
        status = "fail" if outcome == "fail" else "incomplete"
        atomic_json(output_path, {
            **base, "status": status, "reason": receipt["reason"],
            "dependencies": receipt["dependencies"], "campaign": None,
            "controls": None, "partial_artifacts": artifacts,
        })
        return 1 if outcome == "fail" else 2
    controls = (
        {
            "blank_negative": artifacts["blank_negative"],
            "audio_thread_exclusion": artifacts["audio_thread_exclusion"],
            "blank_control_binary": artifacts["blank_control_binary"],
            "audio_control_binary": artifacts["audio_control_binary"],
        }
        if args.require_controls else None
    )
    atomic_json(output_path, {
        **base, "status": "pass", "reason": None, "dependencies": [],
        "campaign": campaign, "controls": controls,
        "measurement_producer": artifacts["measurement_producer"],
    })
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    run = subparsers.add_parser("run-role")
    run.add_argument("--role", required=True, choices=sorted(a3.CAMPAIGN_ROLES))
    run.add_argument("--identity", required=True, type=Path)
    run.add_argument("--budget-receipt", required=True, type=Path)
    run.add_argument("--budget-cold", required=True, type=Path)
    run.add_argument("--budget-warm", required=True, type=Path)
    run.add_argument("--adapter", required=True, type=Path)
    run.add_argument("--output-dir", required=True, type=Path)
    run.add_argument("--timeout-seconds", type=float, default=900)
    run.add_argument(
        "--require-controls", action="store_true",
        help="require a caught blank negative and external audio-thread exclusion proof",
    )
    args = parser.parse_args(argv)
    try:
        if args.command == "run-role":
            return run_role(args)
    except (CampaignError, a3.AcceptanceError, OSError, subprocess.SubprocessError) as error:
        print(f"A3 campaign: FAIL: {error}", file=sys.stderr)
        return 1
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
