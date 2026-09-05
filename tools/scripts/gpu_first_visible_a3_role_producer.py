#!/usr/bin/env python3
"""Shared fail-closed engine for the seven authority-bound A3 role producers.

The role entry points are deliberately thin. This engine snapshots itself and
the configured lifecycle driver before either can contribute evidence. The
driver owns product/host automation and the endpoint observation; this engine
owns the 10+10 request, exact binary/source identity, REAPER/Forge preflights,
artifact confinement, and semantic validation of the returned campaign facts.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import plistlib
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
BUILD_VERIFIER_REQUEST_SCHEMA = "pulp.gpu-first-visible-build-verification-request.v1"
BUILD_VERIFIER_RECEIPT_SCHEMA = "pulp.gpu-first-visible-build-verification-receipt.v1"
SOURCE_BUILD_REQUEST_SCHEMA = "pulp.gpu-first-visible-source-build-request.v1"
SOURCE_BUILD_RECEIPT_SCHEMA = "pulp.gpu-first-visible-source-build-receipt.v1"
BUILD_VERIFIER_SOURCE_PATH = "tools/scripts/gpu_first_visible_a3_build_verifier.py"
TRACE_ANALYZER_SOURCE_PATH = "tools/scripts/gpu_first_visible_a3_trace_analyzer.py"
TRACE_ANALYZER_RECEIPT_SCHEMA = "pulp.gpu-first-visible-prepared-trace-analyzer.v1"
TRACE_ANALYZER_SOURCE_PREFIXES = (
    "experimental/pulp-rs",
    ".agents/skills/trace-sql",
    "tools/packages/tool-registry.json",
    "tools/scripts/release_product_matrix.json",
    "tools/import-design/browser_capture/runtime_manifest.txt",
)
OUTCOME_EXIT = {"pass": 0, "fail": 1, "inconclusive": 2, "skip": 3}
ENDPOINT_BY_ROLE = {
    "pulp-standalone": "native-compositor-presentation",
    "forge-modular-standalone": "native-compositor-presentation",
    "forge-modular-auv2-logic": "native-compositor-presentation",
    "forge-modular-vst3-reaper": "native-compositor-presentation",
    "forge-modular-clap-reaper": "native-compositor-presentation",
    "headless-reference": "headless-capture-complete",
    "constrained-adapter": "native-compositor-presentation",
}
FORMAT_BY_ROLE = {
    "pulp-standalone": {"standalone"},
    "forge-modular-standalone": {"standalone"},
    "forge-modular-auv2-logic": {"auv2"},
    "forge-modular-vst3-reaper": {"vst3"},
    "forge-modular-clap-reaper": {"clap"},
    "headless-reference": {"headless"},
    "constrained-adapter": {"standalone"},
}
ENV_PREFIX_BY_ROLE = {
    "pulp-standalone": "PULP_A3_STANDALONE",
    "forge-modular-standalone": "PULP_A3_FORGE",
    "forge-modular-auv2-logic": "PULP_A3_LOGIC",
    "forge-modular-vst3-reaper": "PULP_A3_REAPER_VST3",
    "forge-modular-clap-reaper": "PULP_A3_REAPER_CLAP",
    "headless-reference": "PULP_A3_HEADLESS",
    "constrained-adapter": "PULP_A3_CONSTRAINED",
}
FORGE_ROLES = frozenset(role for role in ENDPOINT_BY_ROLE if role.startswith("forge-modular-"))
REAPER_ROLES = frozenset({"forge-modular-vst3-reaper", "forge-modular-clap-reaper"})
DAW_ROLES = REAPER_ROLES | {"forge-modular-auv2-logic"}
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
    "lifecycle_provenance", "trace_host_pid",
}
LIVENESS_CHALLENGE_SCHEMA = "pulp.gpu-first-visible-host-liveness-challenge.v1"
LIVENESS_ACK_SCHEMA = "pulp.gpu-first-visible-host-liveness-ack.v1"
LIVENESS_CHALLENGE_KEYS = {
    "schema", "version", "attempt_nonce", "challenge_nonce", "sequence",
    "process_id", "host_pid",
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
BUILD_VERIFIER_RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "control", "outcome", "reason",
    "verification_method", "product_identity", "product_sha256",
    "observed_product_sha256", "marker_sha256",
}
TRACE_ANALYZER_RECEIPT_KEYS = {
    "schema", "version", "pulp_revision", "source_files", "cargo", "rustc",
    "source_snapshot_sha256", "cargo_home_mode", "target_directory_fresh",
    "analyzer_sha256",
}
TRACE_ANALYZER_TOOL_KEYS = {
    "command_path", "resolved_path", "sha256", "version", "retained_path",
    "retained_sha256",
}
SOURCE_BUILD_RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "role", "outcome", "reason",
    "identity", "source_revisions", "build_command", "builder_id",
    "build_started_utc", "build_finished_utc", "driver_sha256",
    "product_path", "product_sha256", "bundle_path", "bundle_tree_sha256",
}


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


def trace_lifetime_evidence_id(
    attempt_nonce: str, challenge_nonce: str, process_id: str,
) -> str:
    material = (
        "pulp-a3-live-trace-v1\0" + attempt_nonce + "\0"
        + challenge_nonce + "\0" + process_id
    ).encode("utf-8")
    return sha256_bytes(material)[:32]


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
    if role in FORGE_ROLES and not (
        isinstance(identity["forge_revision"], str)
        and GIT_REVISION.fullmatch(identity["forge_revision"])
    ):
        raise ProducerError("Forge producer requires an exact Forge revision")
    if role not in FORGE_ROLES and identity["forge_revision"] is not None:
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


def git_revision_files(
    root: Path, revision: str, prefixes: tuple[str, ...], label: str,
) -> list[str]:
    completed = subprocess.run(
        ["/usr/bin/git", "-C", str(root), "ls-tree", "-r", "--name-only",
         revision, "--", *prefixes],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=False, timeout=30,
    )
    if completed.returncode != 0:
        raise ProducerError(
            f"cannot enumerate source-bound {label}: {completed.stderr.strip()}"
        )
    files = sorted(line for line in completed.stdout.splitlines() if line)
    if not files:
        raise ProducerError(f"source-bound {label} is empty")
    return files


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
    # killpg reports "not our process group any more" two ways, not one. ESRCH
    # (ProcessLookupError) means the group is gone; EPERM (PermissionError)
    # means the pid was recycled into a group this process may not signal.
    # Both mean the child we spawned is no longer there, so both end the wait.
    # Catching only ESRCH lets a recycled pid raise EPERM out of teardown and
    # fail an otherwise-passing run.
    gone = (ProcessLookupError, PermissionError)
    process_group = process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except gone:
        return

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process_group, 0)
        except gone:
            break
        time.sleep(0.02)
    else:
        try:
            os.killpg(process_group, signal.SIGKILL)
        except gone:
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


def export_exact_source_tree(
    root: Path, revision: str, destination: Path, label: str,
) -> tuple[Path, Path]:
    """Export one immutable Git tree without ambient worktree/build output."""
    archive = destination.with_suffix(".tar")
    destination.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        ["/usr/bin/git", "archive", "--format=tar", "--output", str(archive), revision],
        cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=180, check=False,
    )
    if completed.returncode != 0 or not archive.is_file() or archive.is_symlink():
        raise ProducerError(f"cannot export exact {label} source tree")
    destination.mkdir(mode=0o700)
    try:
        with tarfile.open(archive, "r:") as handle:
            members = handle.getmembers()
            if any(
                member.name.startswith("/") or ".." in Path(member.name).parts
                or member.islnk()
                for member in members
            ):
                raise ProducerError(f"exact {label} source tree contains an unsafe member")
            handle.extractall(destination, filter="data")
    except (OSError, tarfile.TarError) as error:
        raise ProducerError(f"cannot extract exact {label} source tree") from error
    if not any(destination.iterdir()):
        raise ProducerError(f"exact {label} source tree is empty")
    return archive, destination


def macos_default_deny_build_profile(
    workspace: Path, protected_paths: list[Path],
) -> str:
    """Confine a source build to exported inputs, system tools, and fresh output."""
    readable_roots = (
        workspace,
        Path(sys.executable).resolve().parent.parent,
        Path("/System"),
        Path("/System/Volumes/Preboot"),
        Path("/private/preboot"),
        Path("/usr"),
        Path("/bin"),
        Path("/sbin"),
        Path("/Library/Developer"),
        Path("/Applications/Xcode.app"),
        Path("/opt/homebrew"),
        Path("/usr/local"),
        Path("/private/etc"),
        Path("/private/var/db/timezone"),
        Path("/dev"),
    )
    read_rules = "\n".join(
        f"(allow file-read* (subpath {json.dumps(str(path.resolve()))}))"
        for path in readable_roots if path.exists()
    )
    ambient_roots = (
        Path("/Users"), Path("/Volumes"), Path("/private/tmp"),
        Path("/private/var/folders"), Path("/private/var/tmp"),
    )
    ambient_filters = " ".join(
        f"(require-not (subpath {json.dumps(str(path))}))"
        for path in ambient_roots
    )
    protected_rules = []
    for path in protected_paths:
        resolved = path.resolve()
        selector = "subpath" if resolved.is_dir() else "literal"
        protected_rules.append(
            f"(deny file-read* ({selector} {json.dumps(str(resolved))}))"
        )
    return (
        "(version 1)\n"
        "(deny default)\n"
        "(allow process*)\n"
        "(allow signal)\n"
        "(allow sysctl-read)\n"
        "(allow mach-lookup)\n"
        "(allow ipc-posix-shm)\n"
        "(allow file-read-metadata)\n"
        f"(allow file-read-data (require-all {ambient_filters}))\n"
        f"{read_rules}\n"
        + "\n".join(protected_rules)
        + "\n"
        f"(allow file-write* (subpath {json.dumps(str(workspace.resolve()))}))\n"
        "(deny network*)\n"
    )


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


def require_forge_bundle_identity(
    bundle: Path, product_binary: Path, identity: dict[str, Any],
) -> None:
    plist_path = bundle / "Contents" / "Info.plist"
    try:
        payload = plistlib.loads(regular_file_bytes(plist_path, "Forge Info.plist"))
    except plistlib.InvalidFileException as error:
        raise ProducerError("Forge Info.plist is invalid") from error
    if not isinstance(payload, dict):
        raise ProducerError("Forge Info.plist is not a dictionary")
    executable = payload.get("CFBundleExecutable")
    if (
        executable != product_binary.name
        or payload.get("CFBundleIdentifier") != identity["product_id"]
        or payload.get("CFBundleName") != identity["product_name"]
        or product_binary != (bundle / "Contents" / "MacOS" / str(executable)).resolve()
    ):
        raise ProducerError(
            "Forge Info.plist does not bind the requested bundle, product, and executable identity"
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


def alternate_revision(revision: str) -> str:
    return ("0" if revision[0] != "0" else "1") + revision[1:]


def validate_build_verifier_receipt(
    payload: dict[str, Any], *, verifier_request: dict[str, Any], exit_code: int,
    expected_outcome: str,
) -> None:
    exact_keys(payload, BUILD_VERIFIER_RECEIPT_KEYS, "build-verifier receipt")
    if (
        payload["schema"] != BUILD_VERIFIER_RECEIPT_SCHEMA
        or payload["version"] != 1
        or payload["attempt_nonce"] != verifier_request["attempt_nonce"]
        or payload["control"] != verifier_request["control"]
        or payload["verification_method"] != "embedded-canonical-build-identity"
        or payload["product_identity"] != verifier_request["product_identity"]
        or payload["product_sha256"] != verifier_request["product_sha256"]
        or payload["outcome"] != expected_outcome
        or exit_code != (0 if expected_outcome == "pass" else 1)
    ):
        raise ProducerError("build-verifier result does not bind the closed control")
    if expected_outcome == "pass":
        if (
            payload["reason"] is not None
            or payload["observed_product_sha256"] != verifier_request["product_sha256"]
            or not isinstance(payload["marker_sha256"], str)
            or SHA256.fullmatch(payload["marker_sha256"]) is None
        ):
            raise ProducerError("build-verifier pass lacks embedded product proof")
    elif not isinstance(payload["reason"], str) or not payload["reason"]:
        raise ProducerError("build-verifier negative control lacks a failure reason")


def run_build_verifier(
    *, verifier: Path, request: dict[str, Any], product_snapshot: Path,
    product_digest: str, bundle_digest: str | None, artifact_directory: Path,
) -> tuple[str, list[Path]]:
    tooling = artifact_directory / "tooling"
    tampered_product = tooling / "build-verifier-tampered-product.bin"
    tampered_product.write_bytes(
        regular_file_bytes(product_snapshot, "product snapshot")
        + b"\0pulp-a3-negative-control"
    )
    tampered_product.chmod(0o444)
    product_identity = expected_build_identity(request)
    wrong_identity = dict(product_identity)
    wrong_identity["pulp_revision"] = alternate_revision(
        wrong_identity["pulp_revision"]
    )
    controls = [
        ("tampered-product", tampered_product, product_identity, "fail"),
        ("wrong-source", product_snapshot, wrong_identity, "fail"),
        ("real", product_snapshot, product_identity, "pass"),
    ]
    retained: list[Path] = [tampered_product]
    real_receipt_digest = ""
    for control, product_path, identity, expected_outcome in controls:
        verifier_request = {
            "schema": BUILD_VERIFIER_REQUEST_SCHEMA,
            "version": 1,
            "attempt_nonce": request["attempt_nonce"],
            "control": control,
            "product_identity": identity,
            "product_path": str(product_path),
            "product_sha256": product_digest,
            "bundle_tree_sha256": bundle_digest,
        }
        control_request = tooling / f"build-verifier-{control}.request.json"
        control_receipt = tooling / f"build-verifier-{control}.receipt.json"
        control_stdout = artifact_directory / "logs" / f"build-verifier-{control}.stdout.log"
        control_stderr = artifact_directory / "logs" / f"build-verifier-{control}.stderr.log"
        atomic_json(control_request, verifier_request)
        exit_code = bounded_run(
            [str(verifier), "verify", "--request", str(control_request),
             "--receipt", str(control_receipt)],
            cwd=artifact_directory, environment=dict(os.environ), timeout_seconds=120,
            stdout_path=control_stdout, stderr_path=control_stderr,
        )
        if not control_receipt.is_file() or control_receipt.is_symlink():
            raise ProducerError(f"build verifier omitted its {control} receipt")
        validate_build_verifier_receipt(
            regular_json(control_receipt, f"build-verifier {control} receipt"),
            verifier_request=verifier_request, exit_code=exit_code,
            expected_outcome=expected_outcome,
        )
        retained.extend([
            control_request, control_receipt, control_stdout, control_stderr,
        ])
        if control == "real":
            real_receipt_digest = file_sha256(
                control_receipt, "build-verifier real receipt",
            )
    return real_receipt_digest, retained


def validate_analyzer_tool(value: Any, workspace: Path, label: str) -> Path:
    exact_keys(value, TRACE_ANALYZER_TOOL_KEYS, label)
    command = Path(value["command_path"])
    resolved = Path(value["resolved_path"])
    retained = Path(value["retained_path"])
    if (
        not command.is_absolute() or not command.is_file()
        or command.resolve() != resolved
        or not resolved.is_absolute() or not resolved.is_file()
        or not os.access(command, os.X_OK)
        or value["sha256"] != file_sha256(resolved, label)
        or not retained.is_absolute() or retained.is_symlink()
        or workspace.resolve() not in retained.resolve().parents
        or not retained.is_file() or not os.access(retained, os.X_OK)
        or value["retained_sha256"] != value["sha256"]
        or file_sha256(retained, f"retained {label}") != value["sha256"]
        or not isinstance(value["version"], str) or not value["version"]
        or "\n" in value["version"]
    ):
        raise ProducerError(f"{label} does not bind an exact executable toolchain")
    return retained


def prepare_trace_analyzer(
    *, wrapper: Path, request: dict[str, Any], pulp_root: Path,
    artifact_directory: Path,
) -> tuple[Path, str, list[Path]]:
    workspace = artifact_directory / "tooling" / "prepared-trace-analyzer"
    analyzer = workspace / "pulp"
    receipt_path = workspace / "receipt.json"
    stdout_path = artifact_directory / "logs" / "trace-analyzer-prepare.stdout.log"
    stderr_path = artifact_directory / "logs" / "trace-analyzer-prepare.stderr.log"
    environment = {
        "PULP_A3_PULP_ROOT": str(pulp_root),
        "PULP_A3_PULP_REVISION": request["identity"]["pulp_revision"],
    }
    exit_code = bounded_run(
        [sys.executable, str(wrapper), "prepare", "--workspace", str(workspace),
         "--output", str(analyzer), "--receipt", str(receipt_path)],
        cwd=artifact_directory, environment=environment, timeout_seconds=660,
        stdout_path=stdout_path, stderr_path=stderr_path,
    )
    if exit_code != 0 or not analyzer.is_file() or analyzer.is_symlink():
        raise ProducerError("source-bound trace analyzer preparation failed closed")
    receipt = regular_json(receipt_path, "prepared trace analyzer receipt")
    exact_keys(receipt, TRACE_ANALYZER_RECEIPT_KEYS, "prepared trace analyzer receipt")
    source_files = receipt["source_files"]
    expected_paths = git_revision_files(
        pulp_root, request["identity"]["pulp_revision"],
        TRACE_ANALYZER_SOURCE_PREFIXES, "prepared analyzer sources",
    )
    if not isinstance(source_files, dict) or set(source_files) != set(expected_paths):
        raise ProducerError("prepared analyzer sources have the wrong files")
    revision = request["identity"]["pulp_revision"]
    expected_sources = {
        relative: sha256_bytes(git_file_bytes(
            pulp_root, revision, relative, f"prepared analyzer source {relative}",
        ))
        for relative in expected_paths
    }
    retained_cargo = validate_analyzer_tool(
        receipt["cargo"], workspace, "prepared analyzer Cargo",
    )
    retained_rustc = validate_analyzer_tool(
        receipt["rustc"], workspace, "prepared analyzer rustc",
    )
    source_snapshot = workspace / "source-snapshot.tar"
    analyzer_digest = file_sha256(analyzer, "prepared trace analyzer")
    if (
        receipt["schema"] != TRACE_ANALYZER_RECEIPT_SCHEMA
        or receipt["version"] != 1
        or receipt["pulp_revision"] != revision
        or source_files != expected_sources
        or not isinstance(receipt["source_snapshot_sha256"], str)
        or SHA256.fullmatch(receipt["source_snapshot_sha256"]) is None
        or file_sha256(source_snapshot, "prepared analyzer source snapshot")
        != receipt["source_snapshot_sha256"]
        or receipt["cargo_home_mode"] != "fresh-config-free-linked-locked-cache"
        or receipt["target_directory_fresh"] is not True
        or receipt["analyzer_sha256"] != analyzer_digest
        or not os.access(analyzer, os.X_OK)
    ):
        raise ProducerError("prepared trace analyzer is not sealed to the requested source/toolchain")
    return analyzer, analyzer_digest, [
        receipt_path, stdout_path, stderr_path, source_snapshot,
        retained_cargo, retained_rustc,
    ]


def safe_built_path(root: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ProducerError(f"{label} is missing")
    relative = Path(value)
    lexical = root / relative
    if relative.is_absolute() or ".." in relative.parts or lexical.is_symlink():
        raise ProducerError(f"{label} is unsafe")
    resolved = lexical.resolve()
    if root.resolve() not in resolved.parents:
        raise ProducerError(f"{label} escapes the independent build output")
    return resolved


def run_independent_source_build(
    *, build_driver: Path, build_driver_digest: str, request: dict[str, Any],
    source_authorities: dict[str, tuple[Path, str]], product_digest: str,
    bundle_digest: str | None, artifact_directory: Path,
    measured_paths: list[Path],
) -> tuple[str, list[Path], list[tuple[Path, str, str]]]:
    declared_source_roots = {
        owner: {"path": str(root), "revision": revision}
        for owner, (root, revision) in sorted(source_authorities.items())
    }
    request_path = artifact_directory / "tooling" / "source-build-request.json"
    receipt_path = artifact_directory / "tooling" / "source-build-receipt.json"
    stdout_path = artifact_directory / "logs" / "source-build.stdout.log"
    stderr_path = artifact_directory / "logs" / "source-build.stderr.log"
    if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
        raise ProducerBlocked(
            "independent source proof requires the macOS sandbox used by the M5 campaign",
            "source-build-isolation:macos-sandbox",
        )
    artifact_root = artifact_directory.resolve()
    if any(
        artifact_root == Path(value["path"]).resolve()
        or artifact_root in Path(value["path"]).resolve().parents
        or Path(value["path"]).resolve() in artifact_root.parents
        for value in declared_source_roots.values()
    ):
        raise ProducerError("independent source roots overlap the measured artifact directory")
    with tempfile.TemporaryDirectory(prefix="pulp-a3-independent-build-") as temporary:
        workspace = Path(temporary).resolve()
        output = workspace / "output"
        source_roots: dict[str, dict[str, str]] = {}
        source_archives: dict[str, Path] = {}
        for owner, (root, revision) in sorted(source_authorities.items()):
            archive, snapshot = export_exact_source_tree(
                root, revision, workspace / "sources" / owner,
                f"{owner} source-build",
            )
            source_archives[owner] = archive
            source_roots[owner] = {"path": str(snapshot), "revision": revision}
        local_driver, observed_driver_digest = snapshot_file(
            build_driver, workspace / f"source-build-driver{build_driver.suffix}",
            "isolated source-build driver",
        )
        if observed_driver_digest != build_driver_digest:
            raise ProducerError("isolated source-build driver changed before execution")
        local_request = workspace / "request.json"
        local_receipt = workspace / "receipt.json"
        build_request = {
            "schema": SOURCE_BUILD_REQUEST_SCHEMA,
            "version": 1,
            "attempt_nonce": request["attempt_nonce"],
            "role": request["role"],
            "identity": request["identity"],
            "source_roots": source_roots,
            "output_directory": str(output),
            "bundle_required": request["role"] in FORGE_ROLES,
        }
        atomic_json(local_request, build_request)
        (workspace / "home").mkdir()
        (workspace / "tmp").mkdir()
        path_entries = list(dict.fromkeys((
            str(Path(sys.executable).resolve().parent), "/opt/homebrew/bin",
            "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin",
        )))
        environment = {
            "HOME": str(workspace / "home"),
            "LC_ALL": "C",
            "PATH": ":".join(path_entries),
            "TMPDIR": str(workspace / "tmp"),
        }
        for name in ("DEVELOPER_DIR", "SDKROOT"):
            if os.environ.get(name):
                environment[name] = os.environ[name]
        sandbox_profile = macos_default_deny_build_profile(
            workspace,
            [artifact_root, *[root for root, _ in source_authorities.values()],
             *measured_paths],
        )
        exit_code = bounded_run(
            ["/usr/bin/sandbox-exec", "-p", sandbox_profile, str(local_driver),
             "--request", str(local_request), "--receipt", str(local_receipt)],
            cwd=workspace, environment=environment, timeout_seconds=900,
            stdout_path=stdout_path, stderr_path=stderr_path,
        )
        if not local_receipt.is_file() or local_receipt.is_symlink():
            stdout_tail = regular_file_bytes(
                stdout_path, "source-build driver stdout",
            )[-2048:].decode(errors="replace").strip()
            stderr_tail = regular_file_bytes(
                stderr_path, "source-build driver stderr",
            )[-4096:].decode(errors="replace").strip()
            detail = " | ".join(item for item in (stdout_tail, stderr_tail) if item)
            raise ProducerError(
                f"source-bound build driver omitted its receipt (exit {exit_code})"
                + (f": {detail}" if detail else "")
            )
        receipt = regular_json(local_receipt, "source-build receipt")
        exact_keys(receipt, SOURCE_BUILD_RECEIPT_KEYS, "source-build receipt")
        outcome = receipt["outcome"]
        if (
            receipt["schema"] != SOURCE_BUILD_RECEIPT_SCHEMA
            or receipt["version"] != 1
            or receipt["attempt_nonce"] != request["attempt_nonce"]
            or receipt["role"] != request["role"]
            or receipt["identity"] != request["identity"]
            or receipt["source_revisions"] != {
                owner: value["revision"] for owner, value in source_roots.items()
            }
            or receipt["driver_sha256"] != build_driver_digest
            or outcome not in OUTCOME_EXIT
            or exit_code != OUTCOME_EXIT[outcome]
        ):
            raise ProducerError("source-build receipt differs from the closed request")
        if outcome != "pass":
            if not isinstance(receipt["reason"], str) or not receipt["reason"]:
                raise ProducerError("non-passing source build lacks a reason")
            raise ProducerBlocked(
                receipt["reason"], f"source-build-driver:{request['role']}", outcome,
            )
        if receipt["reason"] is not None:
            raise ProducerError("passing source build cannot carry a reason")
        command = receipt["build_command"]
        if (
            not isinstance(command, list) or not 1 <= len(command) <= 64
            or any(not isinstance(item, str) or not item for item in command)
            or not isinstance(receipt["builder_id"], str) or not receipt["builder_id"]
        ):
            raise ProducerError("source-build receipt lacks a bounded builder identity")
        started = parse_utc(receipt["build_started_utc"], "source build start")
        finished = parse_utc(receipt["build_finished_utc"], "source build finish")
        if finished < started:
            raise ProducerError("source build finishes before it starts")
        if not output.is_dir() or output.is_symlink() or output.resolve() != output:
            raise ProducerError("independent source-build output root is not a fresh directory")
        product = safe_built_path(output, receipt["product_path"], "source-built product")
        observed_product = file_sha256(product, "source-built product")
        if (
            receipt["product_sha256"] != observed_product
            or observed_product != product_digest
            or not os.access(product, os.X_OK)
        ):
            raise ProducerError("independent source build differs from the measured product")
        product_snapshot, snapshot_digest = snapshot_file(
            product, artifact_directory / "identity" / "independent-source-product.bin",
            "independent source-built product",
        )
        if snapshot_digest != observed_product:
            raise ProducerError("independent source-built product changed during snapshot")
        evidence = [request_path, receipt_path, stdout_path, stderr_path, product_snapshot]
        guards = [(
            product_snapshot, observed_product, "independent source-built product snapshot",
        )]
        if bundle_digest is None:
            if receipt["bundle_path"] is not None or receipt["bundle_tree_sha256"] is not None:
                raise ProducerError("non-bundle source build claims a bundle")
            if [entry[0] for entry in directory_entries(output, "source-build output")] != [
                product.relative_to(output).as_posix()
            ]:
                raise ProducerError("non-bundle source build retained unrelated output")
        else:
            bundle = safe_built_path(output, receipt["bundle_path"], "source-built bundle")
            if not bundle.is_dir() or bundle.is_symlink():
                raise ProducerError("source-built bundle is not a regular directory")
            observed_bundle = directory_digest(bundle, "source-built bundle")
            output_entries = [entry[0] for entry in directory_entries(
                output, "source-build output",
            )]
            bundle_prefix = bundle.relative_to(output).as_posix() + "/"
            if any(not relative.startswith(bundle_prefix) for relative in output_entries):
                raise ProducerError("bundle source build retained unrelated output")
            if (
                receipt["bundle_tree_sha256"] != observed_bundle
                or observed_bundle != bundle_digest
                or bundle.resolve() not in product.parents
            ):
                raise ProducerError("independent source-built bundle differs from the measured bundle")
            if request["role"] == "forge-modular-standalone":
                require_forge_bundle_identity(bundle, product, request["identity"])
            bundle_snapshot, _ = snapshot_directory(
                bundle, artifact_directory / "identity" / "independent-source-bundle.tar",
                "independent source-built bundle",
            )
            evidence.append(bundle_snapshot)
            guards.append((
                bundle_snapshot,
                file_sha256(bundle_snapshot, "independent source bundle snapshot"),
                "independent source bundle snapshot",
            ))
        for owner, archive in sorted(source_archives.items()):
            retained_archive, retained_digest = snapshot_file(
                archive,
                artifact_directory / "tooling" / f"source-build-{owner}-source.tar",
                f"independent {owner} source archive",
            )
            evidence.append(retained_archive)
            guards.append((
                retained_archive, retained_digest,
                f"independent {owner} source archive",
            ))
        snapshot_file(local_request, request_path, "source-build request")
        snapshot_file(local_receipt, receipt_path, "source-build receipt")
    return file_sha256(receipt_path, "source-build receipt"), evidence, guards


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
    lifecycle_provenance: list[dict[str, Any]], trace_host_pid: int,
    pulp_root: Path,
) -> tuple[Path, list[Path]]:
    analyzer_environment = dict(os.environ)
    analyzer_environment.update({
        "PULP_A3_PULP_ROOT": str(pulp_root),
        "PULP_A3_PULP_REVISION": request["identity"]["pulp_revision"],
    })
    invalid_trace = artifact_directory / "tooling" / "invalid-trace.pftrace"
    invalid_trace.write_bytes(b"not-a-perfetto-trace")
    invalid_stdout = artifact_directory / "logs" / "trace-analyzer-invalid.stdout.json"
    invalid_stderr = artifact_directory / "logs" / "trace-analyzer-invalid.stderr.log"
    invalid_exit = bounded_run(
        [str(analyzer), "trace", "gpu-startup", "--trace", str(invalid_trace), "--json"],
        cwd=artifact_directory, environment=analyzer_environment, timeout_seconds=60,
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
        cwd=artifact_directory, environment=analyzer_environment, timeout_seconds=60,
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
        or scope["process_pid"] != trace_host_pid
        or trace_host_pid not in host_pids
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


def live_process_executable(pid: int) -> Path:
    if sys.platform == "darwin":
        library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        buffer = ctypes.create_string_buffer(4096)
        library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        library.proc_pidpath.restype = ctypes.c_int
        count = library.proc_pidpath(pid, buffer, len(buffer))
        if count <= 0:
            raise ProducerError(f"host process {pid} has no live executable identity")
        return Path(os.fsdecode(buffer.value)).resolve()
    proc_path = Path("/proc") / str(pid) / "exe"
    if proc_path.exists():
        return proc_path.resolve()
    completed = subprocess.run(
        ["ps", "-p", str(pid), "-o", "comm="], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        timeout=5, check=False,
    )
    value = completed.stdout.strip()
    if completed.returncode != 0 or not value:
        raise ProducerError(f"host process {pid} has no live executable identity")
    return Path(value).resolve()


def live_process_start_identity(pid: int) -> str:
    completed = subprocess.run(
        ["ps", "-p", str(pid), "-o", "lstart="], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        timeout=5, check=False,
    )
    value = " ".join(completed.stdout.split())
    if completed.returncode != 0 or not value:
        raise ProducerError(f"host process {pid} has no live start identity")
    return f"pid={pid};started={value}"


class HostLivenessMonitor:
    """Challenge every claimed lifecycle while its exact host is alive."""

    def __init__(
        self, *, root: Path, request: dict[str, Any], host: Path, host_digest: str,
    ) -> None:
        self.root = root / "liveness"
        self.root.mkdir()
        self.request = request
        self.host = host.resolve()
        self.host_digest = host_digest
        self.nonce = os.urandom(16).hex()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.error: BaseException | None = None
        self.observations: dict[int, dict[str, Any]] = {}
        self.artifacts: list[Path] = []

    def contract(self) -> dict[str, Any]:
        return {
            "schema": LIVENESS_CHALLENGE_SCHEMA,
            "version": 1,
            "attempt_nonce": self.request["attempt_nonce"],
            "challenge_nonce": self.nonce,
            "directory": str(self.root),
            "expected_count": 20,
        }

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        try:
            for sequence in range(20):
                path = self.root / f"challenge-{sequence:02}.json"
                while not path.is_file():
                    if self.stop_event.wait(0.01):
                        return
                challenge = regular_json(path, f"host liveness challenge {sequence}")
                exact_keys(
                    challenge, LIVENESS_CHALLENGE_KEYS,
                    f"host liveness challenge {sequence}",
                )
                if (
                    challenge["schema"] != LIVENESS_CHALLENGE_SCHEMA
                    or challenge["version"] != 1
                    or challenge["attempt_nonce"] != self.request["attempt_nonce"]
                    or challenge["challenge_nonce"] != self.nonce
                    or challenge["sequence"] != sequence
                    or not isinstance(challenge["process_id"], str)
                    or not challenge["process_id"]
                    or isinstance(challenge["host_pid"], bool)
                    or not isinstance(challenge["host_pid"], int)
                    or challenge["host_pid"] <= 1
                ):
                    raise ProducerError(
                        f"host liveness challenge {sequence} differs from the closed request"
                    )
                pid = challenge["host_pid"]
                executable = live_process_executable(pid)
                if (
                    executable != self.host
                    or file_sha256(executable, "live host executable") != self.host_digest
                ):
                    raise ProducerError(
                        f"host liveness challenge {sequence} names the wrong live executable"
                    )
                observation = {
                    "sequence": sequence,
                    "process_id": challenge["process_id"],
                    "host_pid": pid,
                    "process_start_identity": live_process_start_identity(pid),
                    "executable_sha256": self.host_digest,
                }
                ack = {
                    "schema": LIVENESS_ACK_SCHEMA,
                    "version": 1,
                    "attempt_nonce": self.request["attempt_nonce"],
                    "challenge_nonce": self.nonce,
                    **observation,
                }
                ack_path = self.root / f"ack-{sequence:02}.json"
                atomic_json(ack_path, ack)
                self.observations[sequence] = observation
                self.artifacts.extend([path, ack_path])
        except BaseException as error:  # surfaced synchronously by finish()
            self.error = error

    def finish(self) -> tuple[dict[int, dict[str, Any]], list[Path]]:
        self.stop_event.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            raise ProducerError("host liveness monitor did not stop")
        if self.error is not None:
            if isinstance(self.error, ProducerError):
                raise self.error
            raise ProducerError(f"host liveness monitor failed: {self.error}")
        return self.observations, self.artifacts


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
    lifecycle_provenance: list[dict[str, Any]], expected_gpu_evidence_id: str,
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
        if request["role"] == "headless-reference":
            if present is not None:
                raise ProducerError("headless campaign cannot claim native presentation timing")
        elif not finite_nonnegative(present):
            raise ProducerError(f"visible role trial {index} lacks native presentation timing")
    correlation = startup.get("correlation")
    if not isinstance(correlation, dict):
        raise ProducerError("role health result lacks same-instance correlation")
    gpu_id = correlation.get("gpu_evidence_id")
    trace_id = correlation.get("trace_evidence_id")
    if (
        not isinstance(gpu_id, str) or GPU_EVIDENCE_ID.fullmatch(gpu_id) is None
        or gpu_id != expected_gpu_evidence_id
    ):
        raise ProducerError(
            "role health result GPU evidence ID is not derived from the live-host challenge"
        )
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
    liveness: dict[int, dict[str, Any]],
) -> tuple[str, dict[str, Path], list[dict[str, Any]], int | None]:
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
        if (
            not isinstance(lifecycle, list) or len(lifecycle) != 20
            or set(liveness) != set(range(20))
        ):
            raise ProducerError(
                "passing role driver lacks 20 lifecycle provenance rows with producer-observed live hosts"
            )
        observed_lifecycles: dict[str, str] = {}
        process_pids: dict[str, int] = {}
        pid_processes: dict[int, str] = {}
        for index, row in enumerate(lifecycle):
            exact_keys(row, {
                "sequence", "cache_state", "lifecycle_id", "process_id",
                "cache_boundary", "prior_lifecycle_id", "prior_process_id",
                "endpoint_observed", "native_presented", "host_pid",
                "process_start_identity", "executable_sha256",
            }, f"role-driver lifecycle {index}")
            observed = liveness[index]
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
                or row["process_id"] != observed["process_id"]
                or row["host_pid"] != observed["host_pid"]
                or row["process_start_identity"] != observed["process_start_identity"]
                or row["executable_sha256"] != observed["executable_sha256"]
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
            if request["role"] == "headless-reference":
                if row["native_presented"] is not False:
                    raise ProducerError("headless lifecycle cannot claim native presentation")
            elif row["native_presented"] is not True:
                raise ProducerError("visible lifecycle lacks independent native presentation")
        trace_host_pid = receipt["trace_host_pid"]
        if (
            isinstance(trace_host_pid, bool) or not isinstance(trace_host_pid, int)
            or trace_host_pid not in {item["host_pid"] for item in liveness.values()}
        ):
            raise ProducerError("role driver does not bind the trace to a challenged live host")
    else:
        if lifecycle != [] or receipt["trace_host_pid"] is not None:
            raise ProducerError("non-passing role driver cannot retain unvalidated lifecycle claims")
        trace_host_pid = None
    return outcome, paths, lifecycle, trace_host_pid


def run_pinned(role: str, request_path: Path, receipt_path: Path) -> int:
    request = regular_json(request_path, "campaign request")
    artifacts = {key: None for key in sorted(CORE_ARTIFACT_KEYS)}
    try:
        artifact_directory = validate_request(
            request, role=role, request_path=request_path, receipt_path=receipt_path,
        )
        if role == "forge-modular-auv2-logic":
            raise ProducerBlocked(
                "the v2 Logic lifecycle driver is not yet installed on m5",
                "host:logic-pro",
            )
        if role == "constrained-adapter":
            raise ProducerBlocked(
                "the v1 producer request cannot bind protected constrained-adapter authority",
                "product-policy:required-coverage",
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
        forge_root: Path | None = None
        forge_source: dict[str, Any] | None = None
        if role in FORGE_ROLES:
            forge_root = configured_directory("PULP_A3_FORGE_ROOT", "source:forge-root")
            forge_revision = request["identity"]["forge_revision"]
            assert isinstance(forge_revision, str)
            forge_source = validate_source_root(forge_root, forge_revision, "Forge")
            source_guards.append((forge_root, forge_revision, "Forge"))
            source_authorities["forge"] = (forge_root, forge_revision)
        product_binary = configured_file(
            f"{prefix}_PRODUCT_BIN", f"product:{role}",
        )
        host_binary = configured_file(
            f"{prefix}_HOST_BIN", f"host:{role}",
        )
        if role not in DAW_ROLES and product_binary != host_binary:
            raise ProducerError(
                f"{role} product/host must resolve to the same executable"
            )
        driver_source = configured_file(
            f"{prefix}_DRIVER", f"role-driver:{role}",
        )
        analyzer_source = configured_file(
            "PULP_A3_TRACE_ANALYZER", "trace-analyzer:gpu-startup",
        )
        build_verifier_source = configured_file(
            "PULP_A3_BUILD_VERIFIER", "build-verifier:embedded-product-identity",
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
        build_verifier_snapshot, build_verifier_digest = snapshot_file(
            build_verifier_source,
            artifact_directory / "tooling" / f"build-verifier{build_verifier_source.suffix}",
            "embedded product build verifier",
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
            (
                build_verifier_snapshot, build_verifier_digest,
                "build-verifier snapshot",
            ),
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
            "trace_analyzer_wrapper_sha256": analyzer_digest,
            "build_verifier_sha256": build_verifier_digest,
            "producer_support_sha256": support_digest,
        }
        analyzer_source_digest = require_source_file(
            analyzer_source, root=pulp_root,
            revision=request["identity"]["pulp_revision"],
            relative=TRACE_ANALYZER_SOURCE_PATH,
            label="gpu-startup trace analyzer",
        )
        if analyzer_source_digest != analyzer_digest:
            raise ProducerError("trace analyzer snapshot differs from reviewed source")
        build_verifier_source_digest = require_source_file(
            build_verifier_source, root=pulp_root,
            revision=request["identity"]["pulp_revision"],
            relative=BUILD_VERIFIER_SOURCE_PATH,
            label="embedded product build verifier",
        )
        if build_verifier_source_digest != build_verifier_digest:
            raise ProducerError("build verifier snapshot differs from reviewed source")
        prepared_analyzer, prepared_analyzer_digest, prepared_analyzer_evidence = (
            prepare_trace_analyzer(
                wrapper=analyzer_snapshot, request=request, pulp_root=pulp_root,
                artifact_directory=artifact_directory,
            )
        )
        immutable_files.append((
            prepared_analyzer, prepared_analyzer_digest, "prepared trace analyzer",
        ))
        for path in prepared_analyzer_evidence:
            immutable_files.append((
                path, file_sha256(path, f"prepared analyzer artifact {path.name}"),
                f"prepared analyzer artifact {path.name}",
            ))
        preflight.update({
            "trace_analyzer_sha256": prepared_analyzer_digest,
            "trace_analyzer_source": {
                "authority": "pulp",
                "revision": request["identity"]["pulp_revision"],
                "path": TRACE_ANALYZER_SOURCE_PATH,
                "sha256": analyzer_source_digest,
            },
            "build_verifier_source": {
                "authority": "pulp",
                "revision": request["identity"]["pulp_revision"],
                "path": BUILD_VERIFIER_SOURCE_PATH,
                "sha256": build_verifier_source_digest,
            },
        })
        extra_members: list[Path] = []
        extra_members.extend(prepared_analyzer_evidence)
        directory_guards: list[tuple[Path, str, str]] = []
        bundle_tree_digest: str | None = None
        role_context: dict[str, Any] = {"preflight": role}
        if role in REAPER_ROLES:
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
            smoke, reaper_members, reaper_guards = run_reaper_preflight(
                request=request, product_bundle=product_bundle,
                product_binary=product_binary, host_binary=host_binary,
                artifact_directory=artifact_directory, pulp_root=pulp_root,
            )
            extra_members.extend(reaper_members)
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
        elif role == "forge-modular-standalone":
            assert forge_root is not None and forge_source is not None
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
            require_forge_bundle_identity(
                forge_bundle, product_binary, request["identity"],
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
        elif role == "headless-reference":
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

        build_driver_source = configured_file(
            f"{prefix}_BUILD_DRIVER", f"source-build-driver:{role}",
        )
        build_driver_snapshot, build_driver_digest = snapshot_file(
            build_driver_source,
            artifact_directory / "tooling" / f"source-build-driver{build_driver_source.suffix}",
            f"{role} source-build driver",
        )
        build_driver_owner = os.environ.get(
            f"{prefix}_BUILD_DRIVER_SOURCE_OWNER", "pulp",
        )
        if build_driver_owner not in source_authorities:
            raise ProducerError(
                f"{prefix}_BUILD_DRIVER_SOURCE_OWNER does not name an available source authority"
            )
        build_driver_relative = configured_source_path(
            f"{prefix}_BUILD_DRIVER_SOURCE_PATH", f"source-build-driver:{role}",
        )
        build_driver_root, build_driver_revision = source_authorities[build_driver_owner]
        build_driver_source_digest = require_source_file(
            build_driver_source, root=build_driver_root,
            revision=build_driver_revision, relative=build_driver_relative,
            label=f"{role} source-build driver",
        )
        if build_driver_source_digest != build_driver_digest:
            raise ProducerError("source-build driver snapshot differs from reviewed source")
        immutable_files.extend([
            (
                build_driver_source, build_driver_digest,
                "configured source-build driver",
            ),
            (
                build_driver_snapshot, build_driver_digest,
                "source-build driver snapshot",
            ),
        ])
        source_build_digest, source_build_evidence, source_build_guards = (
            run_independent_source_build(
                build_driver=build_driver_snapshot,
                build_driver_digest=build_driver_digest,
                request=request, source_authorities=source_authorities,
                product_digest=product_digest, bundle_digest=bundle_tree_digest,
                artifact_directory=artifact_directory,
                measured_paths=[
                    product_binary,
                    host_binary,
                    *([product_bundle] if role in REAPER_ROLES else []),
                    *([forge_bundle] if role == "forge-modular-standalone" else []),
                ],
            )
        )
        extra_members.extend(source_build_evidence)
        immutable_files.extend(source_build_guards)
        preflight.update({
            "source_build_receipt_sha256": source_build_digest,
            "source_build_driver_source": {
                "authority": build_driver_owner,
                "revision": build_driver_revision,
                "path": build_driver_relative,
                "sha256": build_driver_source_digest,
            },
        })

        build_verification_digest, build_verification_artifacts = run_build_verifier(
            verifier=build_verifier_snapshot, request=request,
            product_snapshot=product_snapshot, product_digest=product_digest,
            bundle_digest=bundle_tree_digest,
            artifact_directory=artifact_directory,
        )
        for path in build_verification_artifacts:
            immutable_files.append((
                path, file_sha256(path, f"build verification artifact {path.name}"),
                f"build verification artifact {path.name}",
            ))
        extra_members.extend(build_verification_artifacts)
        preflight["product_build_verification_sha256"] = build_verification_digest

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
            analyzer_digest=prepared_analyzer_digest,
        )
        validate_build_attestation(
            regular_json(build_attestation_snapshot, "product build attestation"),
            request=request, product_digest=product_digest,
            bundle_digest=bundle_tree_digest, driver_digest=driver_digest,
            analyzer_digest=prepared_analyzer_digest,
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
            "product_build_verification_sha256": build_verification_digest,
            "trace_analyzer_sha256": prepared_analyzer_digest,
        })

        driver_root = artifact_directory / "role-driver-artifacts"
        driver_root.mkdir()
        liveness_monitor = HostLivenessMonitor(
            root=driver_root, request=request, host=host_binary,
            host_digest=host_digest,
        )
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
            "liveness_challenge": liveness_monitor.contract(),
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
        liveness_monitor.start()
        try:
            driver_exit = bounded_run(
                [str(driver_snapshot), "--request", str(driver_request_path),
                 "--receipt", str(driver_receipt_path)],
                cwd=artifact_directory, environment=dict(os.environ), timeout_seconds=480,
                stdout_path=artifact_directory / "logs" / "role-driver.stdout.log",
                stderr_path=artifact_directory / "logs" / "role-driver.stderr.log",
            )
        finally:
            liveness_observations, liveness_artifacts = liveness_monitor.finish()
        extra_members.extend(liveness_artifacts)
        for path in liveness_artifacts:
            immutable_files.append((
                path, file_sha256(path, f"host liveness artifact {path.name}"),
                f"host liveness artifact {path.name}",
            ))
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
        outcome, measured_paths, lifecycle_provenance, trace_host_pid = (
            validate_driver_receipt(
            driver_receipt, request=request, driver_root=driver_root,
            product_digest=product_digest, host_digest=host_digest,
            driver_digest=driver_digest, exit_code=driver_exit,
            liveness=liveness_observations,
            )
        )
        if outcome == "pass":
            assert_host_processes_stopped(lifecycle_provenance)
        if outcome != "pass":
            atomic_json(receipt_path, producer_receipt(
                request, outcome=outcome, reason=driver_receipt["reason"],
                dependencies=driver_receipt["dependencies"], artifacts=artifacts,
            ))
            return OUTCOME_EXIT[outcome]
        trace_process_ids = {
            observation["process_id"]
            for observation in liveness_observations.values()
            if observation["host_pid"] == trace_host_pid
        }
        if len(trace_process_ids) != 1:
            raise ProducerError(
                "challenged trace host does not resolve to one live process identity"
            )
        expected_gpu_evidence_id = trace_lifetime_evidence_id(
            request["attempt_nonce"], liveness_monitor.nonce,
            next(iter(trace_process_ids)),
        )
        trace_observation = next(
            observation for observation in liveness_observations.values()
            if observation["host_pid"] == trace_host_pid
        )
        preflight["trace_lifetime_binding"] = {
            "derivation": "sha256(pulp-a3-live-trace-v1,attempt,challenge,process-id)[:32]",
            "gpu_evidence_id": expected_gpu_evidence_id,
            "process_id": trace_observation["process_id"],
            "host_pid": trace_observation["host_pid"],
            "process_start_identity": trace_observation["process_start_identity"],
        }
        measured_guards = [(
            path, driver_receipt["artifacts"][name]["sha256"],
            f"role-driver {name}",
        ) for name, path in measured_paths.items()]
        health = validate_health_and_trace(
            request=request, health_path=measured_paths["health_result"],
            cold_path=measured_paths["raw_cold"], warm_path=measured_paths["raw_warm"],
            trace_path=measured_paths["trace"],
            lifecycle_provenance=lifecycle_provenance,
            expected_gpu_evidence_id=expected_gpu_evidence_id,
        )
        trace_analysis_path, analyzer_evidence = derive_trace_analysis(
            analyzer=prepared_analyzer, request=request,
            trace_path=measured_paths["trace"],
            health_path=measured_paths["health_result"], health=health,
            artifact_directory=artifact_directory,
            lifecycle_provenance=lifecycle_provenance,
            trace_host_pid=trace_host_pid,
            pulp_root=pulp_root,
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
            ("source-build-driver", build_driver_snapshot),
            ("trace-analyzer", prepared_analyzer),
            ("trace-analyzer-wrapper", analyzer_snapshot),
            ("build-verifier", build_verifier_snapshot),
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
