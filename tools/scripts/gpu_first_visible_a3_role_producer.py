#!/usr/bin/env python3
"""Shared fail-closed engine for the four Pulp-owned A3 role producers.

The role entry points are deliberately thin. This engine snapshots itself and
the configured lifecycle driver before either can contribute evidence. The
driver owns product/host automation and the endpoint observation; this engine
owns the 10+10 request, exact binary/source identity, REAPER/Forge preflights,
artifact confinement, and semantic validation of the returned campaign facts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Any

REQUEST_SCHEMA = "pulp.gpu-first-visible-campaign-request.v1"
PRODUCER_SCHEMA = "pulp.gpu-first-visible-campaign-producer.v1"
DRIVER_REQUEST_SCHEMA = "pulp.gpu-first-visible-role-driver-request.v1"
DRIVER_RECEIPT_SCHEMA = "pulp.gpu-first-visible-role-driver-receipt.v1"
BUILD_ATTESTATION_SCHEMA = "pulp.gpu-first-visible-product-build-attestation.v1"
BUILD_PROVENANCE_SCHEMA = "pulp.gpu-first-visible-local-build-provenance.v1"
OUTCOME_EXIT = {"pass": 0, "fail": 1, "inconclusive": 2, "skip": 3}
ENDPOINT_BY_ROLE = {
    "standalone": "native-compositor-presentation",
    "headless-constrained": "headless-capture-complete",
    "daw": "native-compositor-presentation",
    "forge": "native-compositor-presentation",
}
FORMAT_BY_ROLE = {
    "standalone": {"standalone"},
    "headless-constrained": {"headless"},
    "daw": {"auv2", "vst3", "clap"},
    "forge": {"standalone"},
}
ENV_PREFIX_BY_ROLE = {
    "standalone": "PULP_A3_STANDALONE",
    "headless-constrained": "PULP_A3_HEADLESS",
    "daw": "PULP_A3_REAPER",
    "forge": "PULP_A3_FORGE",
}
REQUEST_KEYS = {
    "schema", "version", "attempt_nonce", "role", "identity",
    "measurement_endpoint", "cold_trial_count", "warm_trial_count",
    "cold_cache_provenance", "warm_cache_provenance", "require_controls",
    "budget", "artifact_directory",
}
IDENTITY_KEYS = {
    "pulp_revision", "forge_revision", "build_id", "product_id", "product_name",
    "plugin_format", "machine_id", "instance_id", "campaign_id",
}
CORE_ARTIFACT_KEYS = {
    "health_result", "raw_cold", "raw_warm", "product_artifact",
    "host_artifact", "trace", "trace_analysis",
}
DRIVER_ARTIFACT_KEYS = {
    "health_result", "raw_cold", "raw_warm", "trace",
}
DRIVER_RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "role", "outcome", "reason",
    "dependencies", "identity", "measurement_endpoint", "product_sha256",
    "host_sha256", "driver_sha256", "artifacts",
    "lifecycle_provenance",
}
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GPU_EVIDENCE_ID = re.compile(r"^[0-9a-f]{32}$")
GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")
OUTPUT_CAP_BYTES = 1024 * 1024
_active_child: subprocess.Popen[bytes] | None = None
BUILD_IDENTITY_KEYS = {
    "pulp_revision", "forge_revision", "build_id", "product_id",
    "product_name", "plugin_format",
}
BUILD_ATTESTATION_KEYS = {
    "schema", "version", "product_identity", "product_sha256",
    "bundle_tree_sha256", "driver_sha256", "trace_analyzer_sha256",
    "provenance_kind", "provenance_receipt_sha256",
}
BUILD_PROVENANCE_KEYS = {
    "schema", "version", "provenance_kind", "product_identity",
    "source_revisions", "source_worktree_status", "product_sha256",
    "bundle_tree_sha256", "driver_sha256", "trace_analyzer_sha256",
    "build_command", "builder_id", "build_started_utc", "build_finished_utc",
}
SOURCE_REVISION_KEYS = {"pulp", "forge"}
LOCAL_PROVENANCE_KIND = "local-clean-exact-head-build-receipt"


class ProducerError(ValueError):
    """A configured role or returned observation violated the closed protocol."""


class ProducerBlocked(RuntimeError):
    """A required external product, host, or driver is unavailable."""

    def __init__(self, reason: str, dependency: str, outcome: str = "inconclusive"):
        super().__init__(reason)
        self.dependency = dependency
        self.outcome = outcome


def exact_keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise ProducerError(f"{label} has the wrong fields")


def regular_file_bytes(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ProducerError(f"{label} is not a readable regular file: {path}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ProducerError(f"{label} is not a regular file: {path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
    finally:
        os.close(descriptor)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha256(path: Path, label: str) -> str:
    return sha256_bytes(regular_file_bytes(path, label))


def regular_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(regular_file_bytes(path, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProducerError(f"{label} is not valid JSON: {path}") from error
    if not isinstance(value, dict):
        raise ProducerError(f"{label} must contain a JSON object")
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


def configured_file(name: str, dependency: str, *, executable: bool = True) -> Path:
    value = os.environ.get(name)
    if not value:
        raise ProducerBlocked(f"{name} is not configured", dependency)
    path = Path(value)
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise ProducerBlocked(
            f"{name} must name an absolute, non-symlink regular file", dependency,
        )
    if executable and not os.access(path, os.X_OK):
        raise ProducerBlocked(f"{name} is not executable", dependency)
    return path.resolve()


def configured_directory(name: str, dependency: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise ProducerBlocked(f"{name} is not configured", dependency)
    path = Path(value)
    if not path.is_absolute() or path.is_symlink() or not path.is_dir():
        raise ProducerBlocked(
            f"{name} must name an absolute, non-symlink directory", dependency,
        )
    return path.resolve()


def configured_source_path(name: str, dependency: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise ProducerBlocked(f"{name} is not configured", dependency)
    path = Path(value)
    if (
        path.is_absolute() or not path.parts
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise ProducerError(f"{name} must name a safe repository-relative source path")
    return path.as_posix()


def snapshot_file(source: Path, destination: Path, label: str) -> tuple[Path, str]:
    data = regular_file_bytes(source, label)
    digest = sha256_bytes(data)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise ProducerError(f"{label} snapshot already exists: {destination}")
    with tempfile.NamedTemporaryFile("wb", dir=destination.parent, delete=False) as handle:
        handle.write(data)
        temporary = Path(handle.name)
    temporary.chmod(0o555 if os.access(source, os.X_OK) else 0o444)
    os.replace(temporary, destination)
    return destination, digest


def artifact_ref(path: Path, run_dir: Path) -> dict[str, str]:
    return {
        "path": path.resolve().relative_to(run_dir.resolve()).as_posix(),
        "sha256": file_sha256(path, "producer artifact"),
    }


def validate_request(
    request: dict[str, Any], *, role: str, request_path: Path, receipt_path: Path,
) -> Path:
    exact_keys(request, REQUEST_KEYS, "campaign request")
    if request["schema"] != REQUEST_SCHEMA or request["version"] != 1:
        raise ProducerError("campaign request has the wrong schema or version")
    if request["role"] != role:
        raise ProducerError(f"{role} producer refuses request role {request['role']!r}")
    if request["measurement_endpoint"] != ENDPOINT_BY_ROLE[role]:
        raise ProducerError(f"{role} request has the wrong measurement endpoint")
    if request["cold_trial_count"] != 10 or request["warm_trial_count"] != 10:
        raise ProducerError("role producer requires exactly 10 cold and 10 warm trials")
    if request["cold_cache_provenance"] != ["fresh-process", "explicit-cache-reset"]:
        raise ProducerError("role producer received an invalid cold cache contract")
    if request["warm_cache_provenance"] != ["same-process-editor-reopen"]:
        raise ProducerError("role producer received an invalid warm cache contract")
    identity = request["identity"]
    exact_keys(identity, IDENTITY_KEYS, "campaign identity")
    if identity["plugin_format"] not in FORMAT_BY_ROLE[role]:
        raise ProducerError(f"{role} producer refuses the requested product format")
    if not GIT_REVISION.fullmatch(identity["pulp_revision"]):
        raise ProducerError("campaign identity lacks an exact Pulp revision")
    if role == "forge" and not (
        isinstance(identity["forge_revision"], str)
        and GIT_REVISION.fullmatch(identity["forge_revision"])
    ):
        raise ProducerError("Forge producer requires an exact Forge revision")
    if role != "forge" and identity["forge_revision"] is not None:
        raise ProducerError(f"{role} producer refuses an unrelated Forge revision")
    artifact_directory = Path(request["artifact_directory"])
    expected = request_path.parent / "adapter-output" / "artifacts"
    if (
        not artifact_directory.is_absolute()
        or artifact_directory.is_symlink()
        or not artifact_directory.is_dir()
        or artifact_directory.resolve() != expected.resolve()
        or receipt_path.parent.resolve() != artifact_directory.resolve()
    ):
        raise ProducerError("role producer artifact directory is not runner-owned")
    return artifact_directory


def producer_receipt(
    request: dict[str, Any], *, outcome: str, reason: str | None,
    dependencies: list[str], artifacts: dict[str, Any],
) -> dict[str, Any]:
    return {
        "schema": PRODUCER_SCHEMA,
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


def git_output(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments], text=True, capture_output=True,
        check=False, timeout=20,
    )
    if completed.returncode != 0:
        raise ProducerError(
            f"cannot verify source identity at {root}: {completed.stderr.strip()}"
        )
    return completed.stdout.strip()


def git_file_bytes(root: Path, revision: str, relative: str, label: str) -> bytes:
    completed = subprocess.run(
        ["git", "-C", str(root), "show", f"{revision}:{relative}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=20,
    )
    if completed.returncode != 0:
        raise ProducerError(
            f"cannot resolve source-bound {label}: {completed.stderr.decode(errors='replace').strip()}"
        )
    return completed.stdout


def require_source_file(
    runtime_path: Path, *, root: Path, revision: str, relative: str, label: str,
) -> str:
    expected = (root / relative).resolve()
    if runtime_path != expected:
        raise ProducerError(f"{label} is not the source-bound checked-in path")
    expected_digest = sha256_bytes(git_file_bytes(root, revision, relative, label))
    if file_sha256(runtime_path, label) != expected_digest:
        raise ProducerError(f"{label} bytes differ from requested source revision")
    return expected_digest


def validate_source_root(root: Path, revision: str, label: str) -> dict[str, str]:
    head = git_output(root, "rev-parse", "HEAD")
    if head != revision:
        raise ProducerError(f"{label} HEAD {head} differs from requested {revision}")
    status = git_output(root, "status", "--porcelain=v1", "--untracked-files=all")
    if status:
        raise ProducerError(f"{label} source checkout has tracked or untracked changes")
    return {"path": str(root), "revision": head, "worktree_status": "clean"}


def terminate_child(process: subprocess.Popen[bytes]) -> None:
    process_group = process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        return

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            break
        time.sleep(0.02)
    else:
        try:
            os.killpg(process_group, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _forward_termination(signum: int, _frame: Any) -> None:
    if _active_child is not None and _active_child.poll() is None:
        terminate_child(_active_child)
    raise SystemExit(128 + signum)


def install_signal_handlers() -> None:
    signal.signal(signal.SIGTERM, _forward_termination)
    signal.signal(signal.SIGINT, _forward_termination)


def bounded_run(
    command: list[str], *, cwd: Path, environment: dict[str, str],
    timeout_seconds: float, stdout_path: Path, stderr_path: Path,
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
        if remaining <= 0 or exceeded.wait(min(0.05, remaining)):
            timed_out = remaining <= 0
            terminate_child(process)
            break
    # A successful driver may exit while host/helper descendants still own
    # its isolated process group and output pipes. Reap that group before any
    # evidence validation so background automation cannot mutate artifacts.
    terminate_child(process)
    for thread in threads:
        thread.join(timeout=2)
    _active_child = None
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path.write_bytes(bytes(buffers[0]))
    stderr_path.write_bytes(bytes(buffers[1]))
    if timed_out:
        raise ProducerBlocked(
            f"{command[0]} exceeded the {timeout_seconds:g}s bound",
            "role-driver:timeout",
        )
    if exceeded.is_set():
        raise ProducerError(f"{command[0]} exceeded the bounded output limit")
    return process.returncode


def directory_entries(
    root: Path, label: str,
) -> list[tuple[str, bytes, int]]:
    entries: list[tuple[str, bytes, int]] = []
    for current, directories, files in os.walk(root, followlinks=False):
        current_path = Path(current)
        for directory in directories:
            path = current_path / directory
            if path.is_symlink() or not stat.S_ISDIR(
                path.stat(follow_symlinks=False).st_mode
            ):
                raise ProducerError(f"{label} contains a non-directory or symlink: {path}")
        for filename in files:
            path = current_path / filename
            if path.is_symlink():
                raise ProducerError(f"{label} contains a symlink: {path}")
            metadata = path.stat(follow_symlinks=False)
            if not stat.S_ISREG(metadata.st_mode):
                raise ProducerError(f"{label} contains a non-regular file: {path}")
            relative = path.relative_to(root).as_posix()
            entries.append((
                relative, regular_file_bytes(path, f"{label} member {relative}"),
                stat.S_IMODE(metadata.st_mode),
            ))
    if not entries:
        raise ProducerError(f"{label} contains no regular files")
    return sorted(entries, key=lambda entry: entry[0])


def directory_tree_digest(entries: list[tuple[str, bytes, int]]) -> str:
    hasher = hashlib.sha256(b"pulp-directory-tree-v1\0")
    for relative, data, mode in entries:
        encoded = relative.encode("utf-8")
        hasher.update(len(encoded).to_bytes(8, "big"))
        hasher.update(encoded)
        hasher.update(mode.to_bytes(4, "big"))
        hasher.update(len(data).to_bytes(8, "big"))
        hasher.update(data)
    return hasher.hexdigest()


def directory_digest(root: Path, label: str) -> str:
    return directory_tree_digest(directory_entries(root, label))


def snapshot_directory(
    source: Path, destination: Path, label: str,
) -> tuple[Path, str]:
    entries = directory_entries(source, label)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise ProducerError(f"{label} snapshot already exists: {destination}")
    with tarfile.open(destination, "w", format=tarfile.PAX_FORMAT) as archive:
        for relative, data, mode in entries:
            info = tarfile.TarInfo(name=relative)
            info.size = len(data)
            info.mtime = 0
            info.mode = mode
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            with tempfile.SpooledTemporaryFile() as handle:
                handle.write(data)
                handle.seek(0)
                archive.addfile(info, handle)
    return destination, directory_tree_digest(entries)


def require_bundle_executable(bundle: Path, product_binary: Path) -> None:
    executable_directory = bundle / "Contents" / "MacOS"
    if not executable_directory.is_dir() or executable_directory.is_symlink():
        raise ProducerError("DAW plugin bundle lacks a regular Contents/MacOS directory")
    candidates = [
        path.resolve()
        for path in executable_directory.iterdir()
        if path.is_file() and not path.is_symlink() and os.access(path, os.X_OK)
    ]
    if candidates != [product_binary]:
        raise ProducerError(
            "DAW product binary is not the sole executable loaded from the exact plugin bundle"
        )


def parse_utc(value: Any, label: str) -> datetime:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ProducerError(f"{label} must be an RFC3339 UTC timestamp")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise ProducerError(f"{label} must be an RFC3339 UTC timestamp") from error
    return parsed


def expected_build_identity(request: dict[str, Any]) -> dict[str, Any]:
    return {key: request["identity"][key] for key in BUILD_IDENTITY_KEYS}


def validate_build_provenance(
    payload: dict[str, Any], *, request: dict[str, Any], product_digest: str,
    bundle_digest: str | None, driver_digest: str, analyzer_digest: str,
) -> None:
    exact_keys(payload, BUILD_PROVENANCE_KEYS, "product build provenance")
    if (
        payload["schema"] != BUILD_PROVENANCE_SCHEMA
        or payload["version"] != 1
        or payload["provenance_kind"] != LOCAL_PROVENANCE_KIND
    ):
        raise ProducerError("product build provenance has invalid protocol identity")
    exact_keys(
        payload["product_identity"], BUILD_IDENTITY_KEYS,
        "build-provenance product identity",
    )
    exact_keys(payload["source_revisions"], SOURCE_REVISION_KEYS, "source revisions")
    exact_keys(
        payload["source_worktree_status"], SOURCE_REVISION_KEYS,
        "source worktree status",
    )
    expected_forge = request["identity"]["forge_revision"]
    expected = {
        "product_identity": expected_build_identity(request),
        "source_revisions": {
            "pulp": request["identity"]["pulp_revision"],
            "forge": expected_forge,
        },
        "source_worktree_status": {
            "pulp": "clean",
            "forge": "clean" if expected_forge is not None else "not-applicable",
        },
        "product_sha256": product_digest,
        "bundle_tree_sha256": bundle_digest,
        "driver_sha256": driver_digest,
        "trace_analyzer_sha256": analyzer_digest,
    }
    if any(payload[key] != value for key, value in expected.items()):
        raise ProducerError(
            "product build provenance does not bind the requested source, product, and driver"
        )
    command = payload["build_command"]
    if (
        not isinstance(command, list) or not 1 <= len(command) <= 64
        or any(not isinstance(item, str) or not item or len(item) > 4096 for item in command)
    ):
        raise ProducerError("product build provenance has an invalid build command")
    builder_id = payload["builder_id"]
    if not isinstance(builder_id, str) or not builder_id or len(builder_id) > 256:
        raise ProducerError("product build provenance has an invalid builder identity")
    started = parse_utc(payload["build_started_utc"], "build_started_utc")
    finished = parse_utc(payload["build_finished_utc"], "build_finished_utc")
    if finished < started:
        raise ProducerError("product build provenance finishes before it starts")


def validate_build_attestation(
    payload: dict[str, Any], *, request: dict[str, Any], product_digest: str,
    bundle_digest: str | None, driver_digest: str, analyzer_digest: str,
    provenance_digest: str,
) -> None:
    exact_keys(payload, BUILD_ATTESTATION_KEYS, "product build attestation")
    if (
        payload["schema"] != BUILD_ATTESTATION_SCHEMA
        or payload["version"] != 1
        or payload["provenance_kind"] != LOCAL_PROVENANCE_KIND
    ):
        raise ProducerError("product build attestation has invalid protocol identity")
    product_identity = payload["product_identity"]
    exact_keys(product_identity, BUILD_IDENTITY_KEYS, "build-attested product identity")
    expected = {
        "product_identity": expected_build_identity(request),
        "product_sha256": product_digest,
        "bundle_tree_sha256": bundle_digest,
        "driver_sha256": driver_digest,
        "trace_analyzer_sha256": analyzer_digest,
        "provenance_receipt_sha256": provenance_digest,
    }
    if any(payload[key] != value for key, value in expected.items()):
        raise ProducerError(
            "product build attestation does not bind the requested source/build identity"
        )


def analyzer_payload(path: Path, exit_code: int, label: str) -> dict[str, Any]:
    payload = regular_json(path, label)
    expected_exit = {"pass": 0, "fail": 1, "unavailable": 2, "unverified": 2}
    verdict = payload.get("verdict")
    if (
        payload.get("schema") != "pulp.trace-gpu-analysis.v1"
        or payload.get("question") != "gpu-startup"
        or verdict not in expected_exit
        or exit_code != expected_exit[verdict]
    ):
        raise ProducerError(f"{label} has invalid protocol identity or exit status")
    return payload


def derive_trace_analysis(
    *, analyzer: Path, request: dict[str, Any], trace_path: Path,
    health_path: Path, health: dict[str, Any], artifact_directory: Path,
    lifecycle_provenance: list[dict[str, Any]],
) -> tuple[Path, list[Path]]:
    invalid_trace = artifact_directory / "tooling" / "invalid-trace.pftrace"
    invalid_trace.write_bytes(b"not-a-perfetto-trace")
    invalid_stdout = artifact_directory / "logs" / "trace-analyzer-invalid.stdout.json"
    invalid_stderr = artifact_directory / "logs" / "trace-analyzer-invalid.stderr.log"
    invalid_exit = bounded_run(
        [str(analyzer), "trace", "gpu-startup", "--trace", str(invalid_trace), "--json"],
        cwd=artifact_directory, environment=dict(os.environ), timeout_seconds=60,
        stdout_path=invalid_stdout, stderr_path=invalid_stderr,
    )
    invalid = analyzer_payload(invalid_stdout, invalid_exit, "invalid-trace analyzer control")
    if (
        invalid["verdict"] != "unavailable"
        or invalid.get("capture_complete") is not False
        or invalid.get("evidence_ids") not in ([], None)
    ):
        raise ProducerError("trace analyzer invalid-input control did not fail closed")

    analyzer_stdout = artifact_directory / "logs" / "trace-analyzer.stdout.json"
    analyzer_stderr = artifact_directory / "logs" / "trace-analyzer.stderr.log"
    analyzer_exit = bounded_run(
        [str(analyzer), "trace", "gpu-startup", "--trace", str(trace_path), "--json"],
        cwd=artifact_directory, environment=dict(os.environ), timeout_seconds=60,
        stdout_path=analyzer_stdout, stderr_path=analyzer_stderr,
    )
    derived = analyzer_payload(analyzer_stdout, analyzer_exit, "campaign trace replay")
    startup = health["startup"]
    correlation = startup["correlation"]
    gpu_id = correlation["gpu_evidence_id"]
    scope = derived.get("category_scope")
    host_pids = {row["host_pid"] for row in lifecycle_provenance}
    if (
        derived["verdict"] not in {"pass", "unverified"}
        or derived.get("capture_complete") is not True
        or derived.get("evidence_ids") != [gpu_id]
        or not isinstance(scope, dict)
        or set(scope) != {"evidence_id", "process_upid", "process_pid"}
        or scope.get("evidence_id") != gpu_id
        or isinstance(scope.get("process_upid"), bool)
        or not isinstance(scope.get("process_upid"), int)
        or scope["process_upid"] < 0
        or isinstance(scope.get("process_pid"), bool)
        or not isinstance(scope.get("process_pid"), int)
        or scope["process_pid"] not in host_pids
    ):
        raise ProducerError(
            "pinned trace replay does not prove the campaign evidence cohort"
        )
    missing = startup["capture"]["missing_trace_categories"]
    trace_digest = file_sha256(trace_path, "same-instance Perfetto trace")
    analysis = {
        "schema": "pulp.gpu-first-visible-campaign-trace.v1",
        "version": 1,
        "question": "gpu-startup",
        "verdict": startup["verdict"],
        "trace_replay_verdict": derived["verdict"],
        "capture_complete": not missing,
        "measurement_endpoint": request["measurement_endpoint"],
        "capture_integrity": "lossless",
        "instrumentation_coverage": "partial" if missing else "complete",
        "missing_trace_categories": missing,
        "campaign_id": request["identity"]["campaign_id"],
        "instance_id": request["identity"]["instance_id"],
        "build_id": request["identity"]["build_id"],
        "gpu_evidence_id": gpu_id,
        "trace_evidence_id": correlation["trace_evidence_id"],
        "trace_sha256": trace_digest,
        "health_result_sha256": file_sha256(health_path, "role health result"),
        "evidence_ids": [gpu_id],
    }
    analysis_path = artifact_directory / "trace-analysis.json"
    atomic_json(analysis_path, analysis)
    return analysis_path, [
        invalid_trace, invalid_stdout, invalid_stderr,
        analyzer_stdout, analyzer_stderr,
    ]


def assert_immutable_files(
    guards: list[tuple[Path, str, str]],
) -> None:
    for path, expected_digest, label in guards:
        if file_sha256(path, label) != expected_digest:
            raise ProducerError(f"{label} changed during the campaign")


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def assert_host_processes_stopped(
    lifecycle_provenance: list[dict[str, Any]],
) -> None:
    live = sorted({
        row["host_pid"] for row in lifecycle_provenance
        if process_exists(row["host_pid"])
    })
    if live:
        raise ProducerError(
            "role driver returned while owned host process IDs remain live: "
            + ",".join(str(pid) for pid in live)
        )


def run_reaper_preflight(
    *, request: dict[str, Any], product_bundle: Path, product_binary: Path,
    host_binary: Path, artifact_directory: Path, pulp_root: Path,
) -> tuple[dict[str, Any], list[Path], list[tuple[Path, str, str]]]:
    smoke = configured_file(
        "PULP_A3_REAPER_SMOKE", "reaper:editor-open-smoke",
    )
    smoke_lua = configured_file(
        "PULP_A3_REAPER_SMOKE_LUA", "reaper:editor-open-smoke-lua",
        executable=False,
    )
    if smoke_lua != smoke.parent / "insert_and_float.lua":
        raise ProducerError(
            "configured REAPER Lua is not the helper used by the smoke harness"
        )
    revision = request["identity"]["pulp_revision"]
    smoke_source_digest = require_source_file(
        smoke, root=pulp_root, revision=revision,
        relative="tools/testing/daw-smoke/reaper_smoke.py",
        label="REAPER smoke harness",
    )
    lua_source_digest = require_source_file(
        smoke_lua, root=pulp_root, revision=revision,
        relative="tools/testing/daw-smoke/insert_and_float.lua",
        label="REAPER editor-open Lua",
    )
    smoke_snapshot, smoke_digest = snapshot_file(
        smoke, artifact_directory / "tooling" / f"reaper-smoke{smoke.suffix}",
        "REAPER smoke harness",
    )
    lua_snapshot, lua_digest = snapshot_file(
        smoke_lua, artifact_directory / "tooling" / "insert-and-float.lua",
        "REAPER editor-open Lua",
    )
    smoke_format = "au" if request["identity"]["plugin_format"] == "auv2" else request["identity"]["plugin_format"]
    command = [
        str(smoke), "--mode", "editor-open", "--format", smoke_format,
        "--plugin-name", request["identity"]["product_name"],
        "--plugin-path", str(product_bundle), "--reaper-bin", str(host_binary),
        "--timeout", os.environ.get("PULP_A3_REAPER_SMOKE_TIMEOUT", "90"),
    ]
    stdout_path = artifact_directory / "logs" / "reaper-smoke.stdout.log"
    stderr_path = artifact_directory / "logs" / "reaper-smoke.stderr.log"
    exit_code = bounded_run(
        command, cwd=artifact_directory, environment=dict(os.environ),
        timeout_seconds=240, stdout_path=stdout_path, stderr_path=stderr_path,
    )
    if file_sha256(smoke, "REAPER smoke harness") != smoke_digest:
        raise ProducerError("REAPER smoke harness changed while it ran")
    if file_sha256(smoke_lua, "REAPER editor-open Lua") != lua_digest:
        raise ProducerError("REAPER editor-open Lua changed while the smoke ran")
    if exit_code == 2:
        raise ProducerBlocked(
            "exact-format REAPER editor-open smoke skipped", "reaper:editor-open-smoke",
            "skip",
        )
    if exit_code == 3:
        raise ProducerBlocked(
            "exact-format REAPER editor-open smoke was inconclusive",
            "reaper:editor-open-smoke",
        )
    if exit_code != 0:
        raise ProducerError(f"exact-format REAPER editor-open smoke exited {exit_code}")
    return ({
        "kind": "reaper-editor-open",
        "format": request["identity"]["plugin_format"],
        "plugin_bundle": str(product_bundle),
        "plugin_binary_sha256": file_sha256(product_binary, "DAW product binary"),
        "host_binary_sha256": file_sha256(host_binary, "REAPER host binary"),
        "smoke_sha256": smoke_digest,
        "smoke_lua_sha256": lua_digest,
        "exit_code": exit_code,
    }, [smoke_snapshot, lua_snapshot, stdout_path, stderr_path], [
        (smoke, smoke_source_digest, "source-bound REAPER smoke harness"),
        (smoke_lua, lua_source_digest, "source-bound REAPER editor-open Lua"),
        (smoke_snapshot, smoke_digest, "REAPER smoke snapshot"),
        (lua_snapshot, lua_digest, "REAPER editor-open Lua snapshot"),
    ])


def validate_artifact_ref(
    ref: Any, *, root: Path, label: str,
) -> tuple[Path, dict[str, str]]:
    exact_keys(ref, {"path", "sha256"}, label)
    assert isinstance(ref, dict)
    relative = Path(ref["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ProducerError(f"{label} path must be safe and relative")
    path = root / relative
    resolved = path.resolve()
    if root.resolve() not in resolved.parents:
        raise ProducerError(f"{label} escapes the driver artifact directory")
    digest = file_sha256(path, label)
    if ref["sha256"] != digest:
        raise ProducerError(f"{label} digest differs from its bytes")
    return path, {"path": str(relative), "sha256": digest}


def finite_nonnegative(value: Any) -> bool:
    return (
        isinstance(value, (int, float)) and not isinstance(value, bool)
        and math.isfinite(value) and value >= 0
    )


def validate_raw(
    payload: dict[str, Any], *, request: dict[str, Any], cache_state: str,
) -> list[dict[str, Any]]:
    exact_keys(
        payload, {"schema", "version", "identity", "cache_state", "samples"},
        f"raw {cache_state}",
    )
    if (
        payload["schema"] != "pulp.gpu-first-visible-campaign-raw.v1"
        or payload["version"] != 1
        or payload["identity"] != request["identity"]
        or payload["cache_state"] != cache_state
        or not isinstance(payload["samples"], list)
        or len(payload["samples"]) != 10
    ):
        raise ProducerError(f"raw {cache_state} does not bind exactly 10 requested trials")
    lifecycles: set[str] = set()
    observed_processes: set[str] = set()
    for index, row in enumerate(payload["samples"]):
        exact_keys(row, {
            "sequence", "duration_ms", "hitch_ms", "lifecycle_id", "process_id",
            "cache_provenance",
        }, f"raw {cache_state} sample {index}")
        if not finite_nonnegative(row["duration_ms"]) or not finite_nonnegative(row["hitch_ms"]):
            raise ProducerError(f"raw {cache_state} sample {index} has invalid timing")
        if not isinstance(row["sequence"], int) or isinstance(row["sequence"], bool):
            raise ProducerError(f"raw {cache_state} sample {index} has invalid sequence")
        for field in ("lifecycle_id", "process_id"):
            if not isinstance(row[field], str) or not row[field] or len(row[field]) > 128:
                raise ProducerError(f"raw {cache_state} sample {index} lacks {field}")
        expected = (
            {"fresh-process", "explicit-cache-reset"}
            if cache_state == "cold" else {"same-process-editor-reopen"}
        )
        if row["cache_provenance"] not in expected:
            raise ProducerError(f"raw {cache_state} sample {index} lacks authentic cache provenance")
        if row["lifecycle_id"] in lifecycles:
            raise ProducerError(f"raw {cache_state} reuses a lifecycle identity")
        lifecycles.add(row["lifecycle_id"])
        if row["cache_provenance"] == "fresh-process":
            if row["process_id"] in observed_processes:
                raise ProducerError(
                    "fresh-process cold trial reuses an earlier process identity"
                )
        observed_processes.add(row["process_id"])
    if len({row["sequence"] for row in payload["samples"]}) != 10:
        raise ProducerError(f"raw {cache_state} sample sequences are not unique")
    return payload["samples"]


def validate_health_and_trace(
    *, request: dict[str, Any], health_path: Path, cold_path: Path,
    warm_path: Path, trace_path: Path,
    lifecycle_provenance: list[dict[str, Any]],
) -> dict[str, Any]:
    cold = validate_raw(regular_json(cold_path, "raw cold"), request=request, cache_state="cold")
    warm = validate_raw(regular_json(warm_path, "raw warm"), request=request, cache_state="warm")
    if {row["lifecycle_id"] for row in cold} & {row["lifecycle_id"] for row in warm}:
        raise ProducerError("campaign reuses a lifecycle identity across cold and warm trials")
    health = regular_json(health_path, "role health result")
    startup = health.get("startup")
    nested_health = health.get("health")
    if not isinstance(startup, dict) or not isinstance(nested_health, dict):
        raise ProducerError("role health result lacks startup or nested health evidence")
    if (
        startup.get("status") != "complete"
        or startup.get("measurement_endpoint") != request["measurement_endpoint"]
        or nested_health.get("run_id") != request["identity"]["campaign_id"]
        or nested_health.get("health_state") != "healthy"
        or nested_health.get("verdict") != "pass"
    ):
        raise ProducerError("role health result does not bind a complete healthy campaign")
    trials = startup.get("trials")
    if not isinstance(trials, list) or len(trials) != 20:
        raise ProducerError("role health result must contain exactly 20 lifecycle trials")
    raw_rows = cold + warm
    for index, (provenance, raw) in enumerate(
        zip(lifecycle_provenance, raw_rows, strict=True)
    ):
        if (
            provenance["sequence"] != raw["sequence"]
            or provenance["lifecycle_id"] != raw["lifecycle_id"]
            or provenance["process_id"] != raw["process_id"]
            or provenance["cache_boundary"] != raw["cache_provenance"]
        ):
            raise ProducerError(
                f"role-driver lifecycle {index} differs from its raw observation"
            )
    for index, (trial, raw) in enumerate(zip(trials, raw_rows, strict=True)):
        if not isinstance(trial, dict):
            raise ProducerError(f"health trial {index} is not an object")
        expected_state = "cold" if index < 10 else "warm"
        if (
            trial.get("sequence") != raw["sequence"]
            or trial.get("cache_state") != expected_state
            or trial.get("lifecycle_id") != raw["lifecycle_id"]
            or trial.get("cache_provenance") != raw["cache_provenance"]
            or trial.get("editor_open_to_first_nonblank_ms") != raw["duration_ms"]
            or trial.get("interaction_hitch_ms") != raw["hitch_ms"]
            or trial.get("content_floor_passed") is not True
            or trial.get("visible_state") in {None, "unknown"}
            or trial.get("verdict") not in {"pass", "fail"}
        ):
            raise ProducerError(f"health trial {index} differs from its authentic raw lifecycle")
        present = trial.get("present_ms")
        if request["role"] == "headless-constrained":
            if present is not None:
                raise ProducerError("headless campaign cannot claim native presentation timing")
        elif not finite_nonnegative(present):
            raise ProducerError(f"visible role trial {index} lacks native presentation timing")
    correlation = startup.get("correlation")
    if not isinstance(correlation, dict):
        raise ProducerError("role health result lacks same-instance correlation")
    gpu_id = correlation.get("gpu_evidence_id")
    trace_id = correlation.get("trace_evidence_id")
    if not isinstance(gpu_id, str) or GPU_EVIDENCE_ID.fullmatch(gpu_id) is None:
        raise ProducerError("role health result lacks a GPU evidence ID")
    if not isinstance(trace_id, str) or not trace_id or trace_id == gpu_id:
        raise ProducerError("role health result lacks a distinct trace evidence ID")
    if not regular_file_bytes(trace_path, "same-instance Perfetto trace"):
        raise ProducerError("same-instance Perfetto trace is empty")
    return health


def deterministic_tar(path: Path, members: list[tuple[str, Path]]) -> None:
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        for name, source in sorted(members):
            data = regular_file_bytes(source, f"host evidence member {name}")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            info.mtime = 0
            info.mode = 0o444
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            with tempfile.SpooledTemporaryFile() as handle:
                handle.write(data)
                handle.seek(0)
                archive.addfile(info, handle)


def validate_driver_receipt(
    receipt: dict[str, Any], *, request: dict[str, Any], driver_root: Path,
    product_digest: str, host_digest: str, driver_digest: str, exit_code: int,
) -> tuple[str, dict[str, Path], list[dict[str, Any]]]:
    exact_keys(receipt, DRIVER_RECEIPT_KEYS, "role-driver receipt")
    if receipt["schema"] != DRIVER_RECEIPT_SCHEMA or receipt["version"] != 1:
        raise ProducerError("role-driver receipt has the wrong schema or version")
    for field in ("attempt_nonce", "role", "identity", "measurement_endpoint"):
        if receipt[field] != request[field]:
            raise ProducerError(f"role-driver receipt {field} differs from the request")
    expected_digests = {
        "product_sha256": product_digest,
        "host_sha256": host_digest,
        "driver_sha256": driver_digest,
    }
    if any(receipt[key] != value for key, value in expected_digests.items()):
        raise ProducerError("role-driver receipt executable identity differs from the pinned inputs")
    outcome = receipt["outcome"]
    if outcome not in OUTCOME_EXIT or exit_code != OUTCOME_EXIT[outcome]:
        raise ProducerError("role-driver exit code disagrees with its outcome")
    dependencies = receipt["dependencies"]
    if (
        not isinstance(dependencies, list) or len(dependencies) > 32
        or len(set(dependencies)) != len(dependencies)
        or any(not isinstance(item, str) or not item for item in dependencies)
    ):
        raise ProducerError("role-driver dependencies are invalid")
    if outcome == "pass":
        if receipt["reason"] is not None or dependencies:
            raise ProducerError("passing role driver cannot carry blockers")
    elif not isinstance(receipt["reason"], str) or not receipt["reason"]:
        raise ProducerError("non-passing role driver requires a reason")
    exact_keys(receipt["artifacts"], DRIVER_ARTIFACT_KEYS, "role-driver artifacts")
    paths: dict[str, Path] = {}
    for name, ref in receipt["artifacts"].items():
        if ref is None:
            if outcome == "pass":
                raise ProducerError(f"passing role driver omitted {name}")
            continue
        paths[name], _ = validate_artifact_ref(
            ref, root=driver_root, label=f"role-driver.{name}",
        )
    lifecycle = receipt["lifecycle_provenance"]
    if outcome == "pass":
        if not isinstance(lifecycle, list) or len(lifecycle) != 20:
            raise ProducerError("passing role driver lacks 20 lifecycle provenance rows")
        observed_lifecycles: dict[str, str] = {}
        process_pids: dict[str, int] = {}
        pid_processes: dict[int, str] = {}
        for index, row in enumerate(lifecycle):
            exact_keys(row, {
                "sequence", "cache_state", "lifecycle_id", "process_id",
                "cache_boundary", "prior_lifecycle_id", "prior_process_id",
                "endpoint_observed", "native_presented", "host_pid",
            }, f"role-driver lifecycle {index}")
            expected_state = "cold" if index < 10 else "warm"
            expected_boundary = (
                {"fresh-process", "explicit-cache-reset"}
                if expected_state == "cold" else {"same-process-editor-reopen"}
            )
            if (
                row["sequence"] != index or row["cache_state"] != expected_state
                or row["cache_boundary"] not in expected_boundary
                or not isinstance(row["lifecycle_id"], str) or not row["lifecycle_id"]
                or not isinstance(row["process_id"], str) or not row["process_id"]
                or isinstance(row["host_pid"], bool)
                or not isinstance(row["host_pid"], int)
                or not 2 <= row["host_pid"] <= 2_147_483_647
                or row["endpoint_observed"] is not True
            ):
                raise ProducerError(f"role-driver lifecycle {index} lacks authentic provenance")
            if expected_state == "cold" and (
                row["prior_lifecycle_id"] is not None
                or row["prior_process_id"] is not None
            ):
                raise ProducerError("cold lifecycle cannot claim an editor-reopen predecessor")
            if expected_state == "warm" and (
                not isinstance(row["prior_lifecycle_id"], str)
                or not row["prior_lifecycle_id"]
                or row["prior_lifecycle_id"] == row["lifecycle_id"]
                or row["prior_process_id"] != row["process_id"]
            ):
                raise ProducerError("warm lifecycle lacks its same-process reopen predecessor")
            if expected_state == "warm" and observed_lifecycles.get(
                row["prior_lifecycle_id"]
            ) != row["process_id"]:
                raise ProducerError(
                    "warm reopen predecessor is not an observed lifecycle in the same process"
                )
            if row["lifecycle_id"] in observed_lifecycles:
                raise ProducerError("role-driver lifecycle identity is not unique")
            observed_lifecycles[row["lifecycle_id"]] = row["process_id"]
            prior_pid = process_pids.setdefault(row["process_id"], row["host_pid"])
            if prior_pid != row["host_pid"]:
                raise ProducerError("one host process identity maps to multiple OS process IDs")
            prior_process = pid_processes.setdefault(row["host_pid"], row["process_id"])
            if prior_process != row["process_id"]:
                raise ProducerError("one OS process ID maps to multiple host process identities")
            if request["role"] == "headless-constrained":
                if row["native_presented"] is not False:
                    raise ProducerError("headless lifecycle cannot claim native presentation")
            elif row["native_presented"] is not True:
                raise ProducerError("visible lifecycle lacks independent native presentation")
    elif lifecycle != []:
        raise ProducerError("non-passing role driver cannot retain unvalidated lifecycle claims")
    return outcome, paths, lifecycle


def run_pinned(role: str, request_path: Path, receipt_path: Path) -> int:
    request = regular_json(request_path, "campaign request")
    artifacts = {key: None for key in sorted(CORE_ARTIFACT_KEYS)}
    try:
        artifact_directory = validate_request(
            request, role=role, request_path=request_path, receipt_path=receipt_path,
        )
        prefix = ENV_PREFIX_BY_ROLE[role]
        pulp_root = configured_directory("PULP_A3_PULP_ROOT", "source:pulp-root")
        pulp_source = validate_source_root(
            pulp_root, request["identity"]["pulp_revision"], "Pulp",
        )
        source_guards = [(
            pulp_root, request["identity"]["pulp_revision"], "Pulp",
        )]
        source_authorities = {"pulp": (
            pulp_root, request["identity"]["pulp_revision"],
        )}
        product_binary = configured_file(
            f"{prefix}_PRODUCT_BIN", f"product:{role}",
        )
        host_binary = configured_file(
            f"{prefix}_HOST_BIN", f"host:{role}",
        )
        if role != "daw" and product_binary != host_binary:
            raise ProducerError(
                f"{role} product/host must resolve to the same executable"
            )
        driver_source = configured_file(
            f"{prefix}_DRIVER", f"role-driver:{role}",
        )
        analyzer_source = configured_file(
            "PULP_A3_TRACE_ANALYZER", "trace-analyzer:gpu-startup",
        )
        product_snapshot, product_digest = snapshot_file(
            product_binary, artifact_directory / "identity" / "product.bin",
            f"{role} product binary",
        )
        host_snapshot, host_digest = snapshot_file(
            host_binary, artifact_directory / "identity" / "host.bin",
            f"{role} host binary",
        )
        driver_snapshot, driver_digest = snapshot_file(
            driver_source,
            artifact_directory / "tooling" / f"lifecycle-driver{driver_source.suffix}",
            f"{role} lifecycle driver",
        )
        analyzer_snapshot, analyzer_digest = snapshot_file(
            analyzer_source,
            artifact_directory / "tooling" / f"trace-analyzer{analyzer_source.suffix}",
            "gpu-startup trace analyzer",
        )
        support_snapshot = Path(__file__).resolve()
        support_digest = file_sha256(support_snapshot, "pinned role-producer support")
        support_source_digest = sha256_bytes(git_file_bytes(
            pulp_root, request["identity"]["pulp_revision"],
            "tools/scripts/gpu_first_visible_a3_role_producer.py",
            "role-producer support",
        ))
        if support_digest != support_source_digest:
            raise ProducerError(
                "pinned role-producer support differs from requested Pulp source"
            )
        immutable_files: list[tuple[Path, str, str]] = [
            (product_binary, product_digest, "configured product binary"),
            (host_binary, host_digest, "configured host binary"),
            (product_snapshot, product_digest, "product snapshot"),
            (host_snapshot, host_digest, "host snapshot"),
            (driver_snapshot, driver_digest, "lifecycle-driver snapshot"),
            (analyzer_snapshot, analyzer_digest, "trace-analyzer snapshot"),
            (support_snapshot, support_digest, "role-producer support snapshot"),
        ]
        preflight: dict[str, Any] = {
            "kind": role,
            "pulp_source": pulp_source,
            "product_runtime_path": str(product_binary),
            "product_sha256": product_digest,
            "host_runtime_path": str(host_binary),
            "host_sha256": host_digest,
            "driver_sha256": driver_digest,
            "trace_analyzer_sha256": analyzer_digest,
            "producer_support_sha256": support_digest,
        }
        extra_members: list[Path] = []
        directory_guards: list[tuple[Path, str, str]] = []
        bundle_tree_digest: str | None = None
        role_context: dict[str, Any] = {"preflight": role}
        if role == "daw":
            product_bundle = configured_directory(
                "PULP_A3_REAPER_PLUGIN_BUNDLE", "product:daw-bundle",
            )
            expected_suffix = {
                "auv2": ".component", "vst3": ".vst3", "clap": ".clap",
            }[request["identity"]["plugin_format"]]
            if product_bundle.suffix != expected_suffix:
                raise ProducerError(
                    "DAW plugin bundle suffix differs from the requested format"
                )
            require_bundle_executable(product_bundle, product_binary)
            bundle_snapshot, bundle_tree_digest = snapshot_directory(
                product_bundle,
                artifact_directory / "identity" / "plugin-bundle.tar",
                "DAW plugin bundle",
            )
            smoke, extra_members, reaper_guards = run_reaper_preflight(
                request=request, product_bundle=product_bundle,
                product_binary=product_binary, host_binary=host_binary,
                artifact_directory=artifact_directory, pulp_root=pulp_root,
            )
            if (
                directory_digest(product_bundle, "DAW plugin bundle")
                != bundle_tree_digest
            ):
                raise ProducerError("DAW plugin bundle changed while the smoke ran")
            bundle_snapshot_digest = file_sha256(
                bundle_snapshot, "DAW plugin bundle snapshot",
            )
            smoke.update({
                "plugin_bundle_tree_sha256": bundle_tree_digest,
                "plugin_bundle_snapshot_sha256": bundle_snapshot_digest,
            })
            extra_members.append(bundle_snapshot)
            directory_guards.append((
                product_bundle, bundle_tree_digest, "DAW plugin bundle",
            ))
            immutable_files.extend(reaper_guards)
            immutable_files.append((
                bundle_snapshot, bundle_snapshot_digest, "DAW plugin bundle snapshot",
            ))
            preflight["reaper_smoke"] = smoke
            role_context.update({
                "host_kind": "reaper",
                "plugin_bundle": str(product_bundle),
                "plugin_format": request["identity"]["plugin_format"],
            })
        elif role == "forge":
            forge_root = configured_directory("PULP_A3_FORGE_ROOT", "source:forge-root")
            forge_bundle = configured_directory(
                "PULP_A3_FORGE_APP_BUNDLE", "product:forge-app-bundle",
            )
            if forge_bundle.suffix != ".app":
                raise ProducerError("Forge app bundle must have the .app suffix")
            if (
                product_binary.parent != forge_bundle / "Contents" / "MacOS"
                or product_binary != host_binary
            ):
                raise ProducerError(
                    "Forge product/host must be the same executable inside the exact app bundle"
                )
            forge_snapshot, bundle_tree_digest = snapshot_directory(
                forge_bundle,
                artifact_directory / "identity" / "forge-app-bundle.tar",
                "Forge app bundle",
            )
            extra_members.append(forge_snapshot)
            directory_guards.append((
                forge_bundle, bundle_tree_digest, "Forge app bundle",
            ))
            forge_source = validate_source_root(
                forge_root, request["identity"]["forge_revision"], "Forge",
            )
            source_guards.append((
                forge_root, request["identity"]["forge_revision"], "Forge",
            ))
            source_authorities["forge"] = (
                forge_root, request["identity"]["forge_revision"],
            )
            forge_snapshot_digest = file_sha256(
                forge_snapshot, "Forge app bundle snapshot",
            )
            immutable_files.append((
                forge_snapshot, forge_snapshot_digest, "Forge app bundle snapshot",
            ))
            preflight.update({
                "forge_source": forge_source,
                "forge_app_bundle_tree_sha256": bundle_tree_digest,
                "forge_app_bundle_snapshot_sha256": forge_snapshot_digest,
            })
            role_context.update({
                "host_kind": "forge-standalone",
                "forge_revision": request["identity"]["forge_revision"],
                "forge_root": str(forge_root),
                "forge_app_bundle": str(forge_bundle),
            })
        elif role == "headless-constrained":
            role_context.update({
                "host_kind": "pulp-headless",
                "completion_source": "headless-capture-complete",
            })
        else:
            role_context.update({
                "host_kind": "pulp-standalone",
                "presentation_source": "independent-native-compositor",
            })

        driver_source_owner = os.environ.get(f"{prefix}_DRIVER_SOURCE_OWNER", "pulp")
        if driver_source_owner not in source_authorities:
            raise ProducerError(
                f"{prefix}_DRIVER_SOURCE_OWNER does not name an available source authority"
            )
        driver_relative = configured_source_path(
            f"{prefix}_DRIVER_SOURCE_PATH", f"driver-source:{role}",
        )
        driver_root, driver_revision = source_authorities[driver_source_owner]
        driver_source_digest = require_source_file(
            driver_source, root=driver_root, revision=driver_revision,
            relative=driver_relative, label=f"{role} lifecycle driver",
        )
        if driver_source_digest != driver_digest:
            raise ProducerError("lifecycle driver snapshot differs from reviewed source")
        preflight["driver_source"] = {
            "authority": driver_source_owner,
            "revision": driver_revision,
            "path": driver_relative,
            "sha256": driver_source_digest,
        }

        build_attestation_source = configured_file(
            f"{prefix}_BUILD_ATTESTATION", f"build-attestation:{role}",
            executable=False,
        )
        build_provenance_source = configured_file(
            f"{prefix}_BUILD_PROVENANCE", f"build-provenance:{role}",
            executable=False,
        )
        build_attestation_snapshot, build_attestation_digest = snapshot_file(
            build_attestation_source,
            artifact_directory / "identity" / "product-build-attestation.json",
            f"{role} product build attestation",
        )
        build_provenance_snapshot, build_provenance_digest = snapshot_file(
            build_provenance_source,
            artifact_directory / "identity" / "product-build-provenance.receipt",
            f"{role} product build provenance receipt",
        )
        build_provenance = regular_json(
            build_provenance_snapshot, "product build provenance",
        )
        validate_build_provenance(
            build_provenance, request=request, product_digest=product_digest,
            bundle_digest=bundle_tree_digest, driver_digest=driver_digest,
            analyzer_digest=analyzer_digest,
        )
        validate_build_attestation(
            regular_json(build_attestation_snapshot, "product build attestation"),
            request=request, product_digest=product_digest,
            bundle_digest=bundle_tree_digest, driver_digest=driver_digest,
            analyzer_digest=analyzer_digest,
            provenance_digest=build_provenance_digest,
        )
        immutable_files.extend([
            (
                build_attestation_source, build_attestation_digest,
                "configured product build attestation",
            ),
            (
                build_provenance_source, build_provenance_digest,
                "configured product build provenance",
            ),
            (
                build_attestation_snapshot, build_attestation_digest,
                "product build attestation snapshot",
            ),
            (
                build_provenance_snapshot, build_provenance_digest,
                "product build provenance snapshot",
            ),
        ])
        extra_members.extend([
            build_attestation_snapshot, build_provenance_snapshot,
        ])
        preflight.update({
            "product_build_attestation_sha256": build_attestation_digest,
            "product_build_provenance_sha256": build_provenance_digest,
        })
        role_context.update({
            "product_build_attestation_sha256": build_attestation_digest,
            "trace_analyzer_sha256": analyzer_digest,
        })

        driver_root = artifact_directory / "role-driver-artifacts"
        driver_root.mkdir()
        driver_request = {
            "schema": DRIVER_REQUEST_SCHEMA,
            "version": 1,
            "attempt_nonce": request["attempt_nonce"],
            "role": role,
            "identity": request["identity"],
            "measurement_endpoint": request["measurement_endpoint"],
            "cold_trial_count": 10,
            "warm_trial_count": 10,
            "cold_cache_provenance": request["cold_cache_provenance"],
            "warm_cache_provenance": request["warm_cache_provenance"],
            "budget": request["budget"],
            "campaign_run_directory": str(request_path.parent),
            "product": {
                "runtime_path": str(product_binary), "sha256": product_digest,
            },
            "host": {"runtime_path": str(host_binary), "sha256": host_digest},
            "driver_sha256": driver_digest,
            "artifact_directory": str(driver_root),
            "role_context": role_context,
        }
        driver_request_path = artifact_directory / "role-driver-request.json"
        driver_receipt_path = artifact_directory / "role-driver-receipt.json"
        atomic_json(driver_request_path, driver_request)
        driver_request_digest = file_sha256(
            driver_request_path, "closed role-driver request",
        )
        immutable_files.append((
            driver_request_path, driver_request_digest, "closed role-driver request",
        ))
        driver_exit = bounded_run(
            [str(driver_snapshot), "--request", str(driver_request_path),
             "--receipt", str(driver_receipt_path)],
            cwd=artifact_directory, environment=dict(os.environ), timeout_seconds=480,
            stdout_path=artifact_directory / "logs" / "role-driver.stdout.log",
            stderr_path=artifact_directory / "logs" / "role-driver.stderr.log",
        )
        assert_immutable_files(immutable_files)
        if file_sha256(product_binary, "configured product binary") != product_digest:
            raise ProducerError("configured product binary changed during the campaign")
        if file_sha256(host_binary, "configured host binary") != host_digest:
            raise ProducerError("configured host binary changed during the campaign")
        for directory, expected_digest, label in directory_guards:
            if directory_digest(directory, label) != expected_digest:
                raise ProducerError(f"{label} changed during the campaign")
        for source_root, source_revision, source_label in source_guards:
            validate_source_root(source_root, source_revision, source_label)
        if not driver_receipt_path.is_file() or driver_receipt_path.is_symlink():
            raise ProducerError(f"role driver exited {driver_exit} without its receipt")
        driver_receipt = regular_json(driver_receipt_path, "role-driver receipt")
        driver_receipt_digest = file_sha256(
            driver_receipt_path, "closed role-driver receipt",
        )
        outcome, measured_paths, lifecycle_provenance = validate_driver_receipt(
            driver_receipt, request=request, driver_root=driver_root,
            product_digest=product_digest, host_digest=host_digest,
            driver_digest=driver_digest, exit_code=driver_exit,
        )
        if outcome == "pass":
            assert_host_processes_stopped(lifecycle_provenance)
        if outcome != "pass":
            atomic_json(receipt_path, producer_receipt(
                request, outcome=outcome, reason=driver_receipt["reason"],
                dependencies=driver_receipt["dependencies"], artifacts=artifacts,
            ))
            return OUTCOME_EXIT[outcome]
        measured_guards = [(
            path, driver_receipt["artifacts"][name]["sha256"],
            f"role-driver {name}",
        ) for name, path in measured_paths.items()]
        health = validate_health_and_trace(
            request=request, health_path=measured_paths["health_result"],
            cold_path=measured_paths["raw_cold"], warm_path=measured_paths["raw_warm"],
            trace_path=measured_paths["trace"],
            lifecycle_provenance=lifecycle_provenance,
        )
        trace_analysis_path, analyzer_evidence = derive_trace_analysis(
            analyzer=analyzer_snapshot, request=request,
            trace_path=measured_paths["trace"],
            health_path=measured_paths["health_result"], health=health,
            artifact_directory=artifact_directory,
            lifecycle_provenance=lifecycle_provenance,
        )
        measured_paths["trace_analysis"] = trace_analysis_path
        extra_members.extend(analyzer_evidence)
        assert_immutable_files(immutable_files)
        assert_immutable_files(measured_guards)
        assert_host_processes_stopped(lifecycle_provenance)
        if file_sha256(driver_receipt_path, "closed role-driver receipt") != driver_receipt_digest:
            raise ProducerError("closed role-driver receipt changed after validation")
        for directory, expected_digest, label in directory_guards:
            if directory_digest(directory, label) != expected_digest:
                raise ProducerError(f"{label} changed during trace replay")
        for source_root, source_revision, source_label in source_guards:
            validate_source_root(source_root, source_revision, source_label)
        preflight_path = artifact_directory / "preflight.json"
        atomic_json(preflight_path, preflight)
        host_archive = artifact_directory / "identity" / "host-evidence.tar"
        members = [
            ("host-executable", host_snapshot),
            ("lifecycle-driver", driver_snapshot),
            ("trace-analyzer", analyzer_snapshot),
            ("producer-support.py", support_snapshot),
            ("driver-request.json", driver_request_path),
            ("driver-receipt.json", driver_receipt_path),
            ("preflight.json", preflight_path),
        ]
        members.extend((f"preflight/{path.name}", path) for path in extra_members)
        deterministic_tar(host_archive, members)
        run_dir = request_path.parent
        artifacts.update({
            "product_artifact": artifact_ref(product_snapshot, run_dir),
            "host_artifact": artifact_ref(host_archive, run_dir),
            **{name: artifact_ref(path, run_dir) for name, path in measured_paths.items()},
        })
        atomic_json(receipt_path, producer_receipt(
            request, outcome="pass", reason=None, dependencies=[], artifacts=artifacts,
        ))
        return 0
    except ProducerBlocked as error:
        atomic_json(receipt_path, producer_receipt(
            request, outcome=error.outcome, reason=str(error),
            dependencies=[error.dependency], artifacts=artifacts,
        ))
        return OUTCOME_EXIT[error.outcome]
    except (ProducerError, OSError, subprocess.SubprocessError) as error:
        atomic_json(receipt_path, producer_receipt(
            request, outcome="fail", reason=str(error),
            dependencies=[f"role-producer:{role}:invalid-evidence"], artifacts=artifacts,
        ))
        return 1


def main_entry(role: str, argv: list[str] | None = None) -> int:
    install_signal_handlers()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--pinned", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.pinned:
        return run_pinned(role, args.request.resolve(), args.receipt.resolve())
    try:
        request = regular_json(args.request, "campaign request")
        artifact_directory = validate_request(
            request, role=role, request_path=args.request.resolve(),
            receipt_path=args.receipt.resolve(),
        )
        pinned, _ = snapshot_file(
            Path(__file__).resolve(), artifact_directory / "tooling" / "role-producer-support.py",
            "role-producer support",
        )
        completed = subprocess.run([
            sys.executable, str(pinned), "--fixed-role", role,
            "--request", str(args.request.resolve()), "--receipt", str(args.receipt.resolve()),
            "--pinned",
        ], check=False)
        return completed.returncode
    except (KeyError, ProducerError, OSError, subprocess.SubprocessError) as error:
        print(f"A3 {role} producer: FAIL: {error}", file=sys.stderr)
        return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixed-role", required=True, choices=sorted(ENDPOINT_BY_ROLE))
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--pinned", action="store_true")
    args = parser.parse_args(argv)
    forwarded = ["--request", str(args.request), "--receipt", str(args.receipt)]
    if args.pinned:
        forwarded.append("--pinned")
    return main_entry(args.fixed_role, forwarded)


if __name__ == "__main__":
    raise SystemExit(main())
