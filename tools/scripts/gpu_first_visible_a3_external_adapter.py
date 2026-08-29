#!/usr/bin/env python3
"""Pin and run one real product producer for an A3 campaign role.

This adapter is deliberately product-agnostic.  The runner snapshots this
file; this file then snapshots the role-specific producer before asking it to
perform the real standalone, headless, DAW, or Forge lifecycle.  It does not
infer cache state, presentation, trace correlation, or product identity.  A
producer that cannot measure those facts must return a durable non-pass result.

When the campaign request selects the two external controls, this adapter also
pins and runs the focused blank-frame and audio-thread harness binaries.  That
keeps ``--require-controls`` executable without allowing a producer to replace
those independent checks with an assertion of its own.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

REQUEST_SCHEMA = "pulp.gpu-first-visible-campaign-request.v1"
PRODUCER_SCHEMA = "pulp.gpu-first-visible-campaign-producer.v1"
ADAPTER_SCHEMA = "pulp.gpu-first-visible-campaign-adapter.v1"
OUTCOME_EXIT = {"pass": 0, "fail": 1, "inconclusive": 2, "skip": 3}
OUTPUT_CAP_BYTES = 1024 * 1024
REQUEST_KEYS = {
    "schema", "version", "attempt_nonce", "role", "identity",
    "measurement_endpoint", "cold_trial_count", "warm_trial_count",
    "cold_cache_provenance", "warm_cache_provenance", "require_controls",
    "budget", "artifact_directory",
}
CORE_ARTIFACT_KEYS = {
    "health_result", "raw_cold", "raw_warm", "product_artifact",
    "host_artifact", "trace", "trace_analysis",
}
ADAPTER_ARTIFACT_KEYS = CORE_ARTIFACT_KEYS | {
    "blank_negative", "audio_thread_exclusion", "measurement_producer",
    "blank_control_binary", "audio_control_binary",
}
PRODUCER_KEYS = {
    "schema", "version", "attempt_nonce", "role", "outcome", "reason",
    "dependencies", "identity", "measurement_endpoint", "artifacts",
}


class AdapterError(ValueError):
    """A configured producer or control violated the campaign protocol."""


class AdapterBlocked(RuntimeError):
    """An external final-head prerequisite is not available on this host."""

    def __init__(self, reason: str, dependency: str):
        super().__init__(reason)
        self.dependency = dependency


_active_child: subprocess.Popen[bytes] | None = None


def exact_keys(value: Any, keys: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != keys:
        raise AdapterError(f"{label} has the wrong fields")


def regular_file_bytes(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise AdapterError(f"{label} is not a readable regular file: {path}") from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise AdapterError(f"{label} is not a regular file: {path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def regular_file_sha256(path: Path, label: str) -> str:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise AdapterError(f"{label} is not a readable regular file: {path}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise AdapterError(f"{label} is not a regular file: {path}")
        digest = hashlib.sha256()
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                return digest.hexdigest()
            digest.update(chunk)
    finally:
        os.close(descriptor)


def regular_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(regular_file_bytes(path, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AdapterError(f"{label} is not valid JSON: {path}") from error
    if not isinstance(value, dict):
        raise AdapterError(f"{label} must contain a JSON object")
    return value


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False,
    ) as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    os.replace(temporary, path)


def validate_request(request: dict[str, Any], request_path: Path) -> Path:
    exact_keys(request, REQUEST_KEYS, "campaign request")
    if request["schema"] != REQUEST_SCHEMA or request["version"] != 1:
        raise AdapterError("campaign request has the wrong schema or version")
    if request["role"] not in {"standalone", "headless-constrained", "daw", "forge"}:
        raise AdapterError("campaign request has an unknown role")
    if request["cold_trial_count"] != 10 or request["warm_trial_count"] != 10:
        raise AdapterError("campaign request does not require exactly 10 cold and 10 warm trials")
    if request["cold_cache_provenance"] != ["fresh-process", "explicit-cache-reset"]:
        raise AdapterError("campaign request changed the cold cache provenance contract")
    if request["warm_cache_provenance"] != ["same-process-editor-reopen"]:
        raise AdapterError("campaign request changed the warm cache provenance contract")
    artifact_directory = Path(request["artifact_directory"])
    expected = request_path.parent / "adapter-output" / "artifacts"
    if (
        not artifact_directory.is_absolute()
        or artifact_directory.is_symlink()
        or not artifact_directory.is_dir()
        or artifact_directory.resolve() != expected.resolve()
    ):
        raise AdapterError("campaign artifact directory is not the runner-owned directory")
    return artifact_directory


def empty_artifacts() -> dict[str, Any]:
    return {key: None for key in sorted(ADAPTER_ARTIFACT_KEYS)}


def adapter_receipt(
    request: dict[str, Any], *, outcome: str, reason: str | None,
    dependencies: list[str], artifacts: dict[str, Any],
) -> dict[str, Any]:
    return {
        "schema": ADAPTER_SCHEMA,
        "version": 1,
        "attempt_nonce": request["attempt_nonce"],
        "role": request["role"],
        "outcome": outcome,
        "reason": reason,
        "dependencies": sorted(set(dependencies)),
        "identity": request["identity"],
        "measurement_endpoint": request["measurement_endpoint"],
        "artifacts": artifacts,
    }


def configured_executable(name: str, dependency: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise AdapterBlocked(f"{name} is not configured", dependency)
    path = Path(value)
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise AdapterBlocked(
            f"{name} must name an absolute, non-symlink regular executable",
            dependency,
        )
    if not os.access(path, os.X_OK):
        raise AdapterBlocked(f"{name} is not executable", dependency)
    return path.resolve()


def pin_executable(source: Path, destination: Path, label: str) -> tuple[Path, str]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise AdapterError(f"{label} snapshot already exists: {destination}")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(source, flags)
    except OSError as error:
        raise AdapterError(f"{label} is not a readable regular file: {source}") from error
    digest = hashlib.sha256()
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise AdapterError(f"{label} is not a regular file: {source}")
        with tempfile.NamedTemporaryFile("wb", dir=destination.parent, delete=False) as handle:
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                handle.write(chunk)
            temporary = Path(handle.name)
    finally:
        os.close(descriptor)
    temporary.chmod(0o555)
    os.replace(temporary, destination)
    return destination, digest.hexdigest()


def artifact_ref(path: Path, run_dir: Path) -> dict[str, str]:
    return {
        "path": path.resolve().relative_to(run_dir.resolve()).as_posix(),
        "sha256": regular_file_sha256(path, "adapter artifact"),
    }


def validate_artifact_ref(
    ref: Any, artifact_directory: Path, run_dir: Path, label: str,
) -> None:
    exact_keys(ref, {"path", "sha256"}, label)
    assert isinstance(ref, dict)
    relative = Path(ref["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise AdapterError(f"{label} path must be safe and relative")
    path = run_dir / relative
    resolved = path.resolve()
    if artifact_directory.resolve() not in resolved.parents:
        raise AdapterError(f"{label} must stay beneath the issued artifact directory")
    if ref["sha256"] != regular_file_sha256(path, label):
        raise AdapterError(f"{label} digest differs from its immutable bytes")


def terminate_child(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def _forward_termination(signum: int, _frame: Any) -> None:
    if _active_child is not None and _active_child.poll() is None:
        terminate_child(_active_child)
    raise SystemExit(128 + signum)


def run_bounded(
    command: list[str], *, environment: dict[str, str], cwd: Path,
    timeout_seconds: float, log_prefix: Path,
) -> int:
    global _active_child
    process = subprocess.Popen(
        command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True,
    )
    _active_child = process
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
            terminate_child(process)
            break
        if exceeded.wait(min(0.05, remaining)):
            terminate_child(process)
            break
    for thread in threads:
        thread.join(timeout=2)
    _active_child = None
    log_prefix.parent.mkdir(parents=True, exist_ok=True)
    log_prefix.with_suffix(".stdout.log").write_bytes(bytes(buffers[0]))
    log_prefix.with_suffix(".stderr.log").write_bytes(bytes(buffers[1]))
    if timed_out:
        raise AdapterBlocked(
            f"{command[0]} exceeded the {timeout_seconds:g}s bound",
            "campaign-producer:timeout",
        )
    if exceeded.is_set():
        raise AdapterError(
            f"{command[0]} exceeded {OUTPUT_CAP_BYTES} bytes per output stream"
        )
    return process.returncode


def validate_producer_receipt(
    receipt: dict[str, Any], *, request: dict[str, Any],
    artifact_directory: Path, run_dir: Path, exit_code: int,
) -> tuple[str, dict[str, Any]]:
    exact_keys(receipt, PRODUCER_KEYS, "measurement producer receipt")
    if receipt["schema"] != PRODUCER_SCHEMA or receipt["version"] != 1:
        raise AdapterError("measurement producer receipt has the wrong schema or version")
    for field in ("attempt_nonce", "role", "identity", "measurement_endpoint"):
        if receipt[field] != request[field]:
            raise AdapterError(f"measurement producer receipt {field} differs from the request")
    outcome = receipt["outcome"]
    if outcome not in OUTCOME_EXIT or exit_code != OUTCOME_EXIT[outcome]:
        raise AdapterError("measurement producer exit code disagrees with its outcome")
    dependencies = receipt["dependencies"]
    if (
        not isinstance(dependencies, list)
        or len(dependencies) > 32
        or len(set(dependencies)) != len(dependencies)
        or any(not isinstance(item, str) or not item for item in dependencies)
    ):
        raise AdapterError("measurement producer dependencies are invalid")
    if outcome == "pass":
        if receipt["reason"] is not None or dependencies:
            raise AdapterError("passing measurement producer cannot carry blockers")
    elif not isinstance(receipt["reason"], str) or not receipt["reason"]:
        raise AdapterError("non-passing measurement producer requires a reason")
    artifacts = receipt["artifacts"]
    exact_keys(artifacts, CORE_ARTIFACT_KEYS, "measurement producer artifacts")
    for name, ref in artifacts.items():
        if ref is None:
            if outcome == "pass":
                raise AdapterError(f"passing measurement producer omitted {name}")
            continue
        validate_artifact_ref(ref, artifact_directory, run_dir, f"producer.{name}")
    return outcome, artifacts


def run_control(
    *, request: dict[str, Any], artifact_directory: Path, run_dir: Path,
    environment_name: str, dependency: str, snapshot_name: str,
    receipt_name: str, receipt_environment: str, test_filter: str,
) -> tuple[dict[str, str], dict[str, str]]:
    source = configured_executable(environment_name, dependency)
    suffix = source.suffix
    pinned, digest = pin_executable(
        source, artifact_directory / "tooling" / f"{snapshot_name}{suffix}",
        environment_name,
    )
    receipt_path = artifact_directory / "controls" / receipt_name
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment[receipt_environment] = str(receipt_path.resolve())
    exit_code = run_bounded(
        [str(pinned), test_filter], environment=environment,
        cwd=artifact_directory, timeout_seconds=300,
        log_prefix=artifact_directory / "logs" / snapshot_name,
    )
    if exit_code != 0:
        raise AdapterError(f"{snapshot_name} exited {exit_code}")
    if not receipt_path.is_file() or receipt_path.is_symlink():
        raise AdapterError(f"{snapshot_name} did not write its focused receipt")
    binary_ref = artifact_ref(pinned, run_dir)
    if binary_ref["sha256"] != digest:
        raise AdapterError(f"{snapshot_name} snapshot changed while it ran")
    return artifact_ref(receipt_path, run_dir), binary_ref


def run(request_path: Path, receipt_path: Path) -> int:
    request = regular_json(request_path, "campaign request")
    artifact_directory = validate_request(request, request_path)
    run_dir = request_path.parent
    artifacts = empty_artifacts()
    role_dependency = f"campaign-producer:{request['role']}"
    try:
        producer_source = configured_executable(
            "PULP_A3_CAMPAIGN_PRODUCER", role_dependency,
        )
    except AdapterBlocked as error:
        atomic_json(receipt_path, adapter_receipt(
            request, outcome="inconclusive", reason=str(error),
            dependencies=[error.dependency], artifacts=artifacts,
        ))
        return OUTCOME_EXIT["inconclusive"]

    producer_suffix = producer_source.suffix
    producer, producer_digest = pin_executable(
        producer_source,
        artifact_directory / "tooling" / f"measurement-producer{producer_suffix}",
        "campaign measurement producer",
    )
    artifacts["measurement_producer"] = artifact_ref(producer, run_dir)
    producer_receipt_path = artifact_directory / "producer-receipt.json"
    try:
        producer_exit = run_bounded(
            [str(producer), "--request", str(request_path.resolve()),
             "--receipt", str(producer_receipt_path.resolve())],
            environment=dict(os.environ), cwd=artifact_directory,
            timeout_seconds=780,
            log_prefix=artifact_directory / "logs" / "measurement-producer",
        )
        if artifacts["measurement_producer"]["sha256"] != producer_digest:
            raise AdapterError("measurement producer snapshot changed while it ran")
        if not producer_receipt_path.is_file() or producer_receipt_path.is_symlink():
            raise AdapterError(
                f"measurement producer exited {producer_exit} without its receipt"
            )
        producer_receipt = regular_json(
            producer_receipt_path, "measurement producer receipt",
        )
        outcome, producer_artifacts = validate_producer_receipt(
            producer_receipt, request=request, artifact_directory=artifact_directory,
            run_dir=run_dir, exit_code=producer_exit,
        )
        artifacts.update(producer_artifacts)
    except AdapterBlocked as error:
        atomic_json(receipt_path, adapter_receipt(
            request, outcome="inconclusive", reason=str(error),
            dependencies=[error.dependency], artifacts=artifacts,
        ))
        return OUTCOME_EXIT["inconclusive"]
    except (AdapterError, OSError, subprocess.SubprocessError) as error:
        atomic_json(receipt_path, adapter_receipt(
            request, outcome="fail", reason=str(error),
            dependencies=[f"{role_dependency}:invalid-evidence"], artifacts=artifacts,
        ))
        return OUTCOME_EXIT["fail"]

    if outcome != "pass":
        atomic_json(receipt_path, adapter_receipt(
            request, outcome=outcome, reason=producer_receipt["reason"],
            dependencies=producer_receipt["dependencies"], artifacts=artifacts,
        ))
        return OUTCOME_EXIT[outcome]

    if request["require_controls"]:
        try:
            (artifacts["blank_negative"], artifacts["blank_control_binary"]) = run_control(
                request=request, artifact_directory=artifact_directory, run_dir=run_dir,
                environment_name="PULP_A3_BLANK_CONTROL_BIN",
                dependency="control:blank-negative-binary",
                snapshot_name="blank-negative-control",
                receipt_name="blank-negative.json",
                receipt_environment="PULP_A3_BLANK_NEGATIVE_RECEIPT_PATH",
                test_filter="exact Standalone product catches the seeded transparent first frame",
            )
            (
                artifacts["audio_thread_exclusion"],
                artifacts["audio_control_binary"],
            ) = run_control(
                request=request, artifact_directory=artifact_directory, run_dir=run_dir,
                environment_name="PULP_A3_AUDIO_CONTROL_BIN",
                dependency="control:audio-thread-exclusion-binary",
                snapshot_name="audio-thread-exclusion-control",
                receipt_name="audio-thread-exclusion.json",
                receipt_environment="PULP_A3_AUDIO_THREAD_EXCLUSION_RECEIPT_PATH",
                test_filter=(
                    "external harness observes every GPU health entry point off a registered "
                    "audio thread"
                ),
            )
        except AdapterBlocked as error:
            atomic_json(receipt_path, adapter_receipt(
                request, outcome="inconclusive", reason=str(error),
                dependencies=[error.dependency], artifacts=artifacts,
            ))
            return OUTCOME_EXIT["inconclusive"]
        except (AdapterError, OSError, subprocess.SubprocessError) as error:
            atomic_json(receipt_path, adapter_receipt(
                request, outcome="fail", reason=str(error),
                dependencies=["control:invalid-evidence"], artifacts=artifacts,
            ))
            return OUTCOME_EXIT["fail"]

    atomic_json(receipt_path, adapter_receipt(
        request, outcome="pass", reason=None, dependencies=[], artifacts=artifacts,
    ))
    return OUTCOME_EXIT["pass"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, _forward_termination)
    signal.signal(signal.SIGINT, _forward_termination)
    try:
        return run(args.request.resolve(), args.receipt.resolve())
    except (AdapterError, OSError, subprocess.SubprocessError) as error:
        print(f"A3 external adapter: FAIL: {error}", file=sys.stderr)
        return OUTCOME_EXIT["fail"]


if __name__ == "__main__":
    raise SystemExit(main())
