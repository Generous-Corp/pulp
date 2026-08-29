#!/usr/bin/env python3
"""Ratify and verify the A3 product trace-producer overhead control."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
import statistics
import sys
import tarfile
import tempfile
import threading
from pathlib import Path
from typing import Any

import gpu_first_visible_a3_trace_producer_overhead_analyzer as trace_replay
import gpu_first_visible_a3_role_producer as role_support

RAW_SCHEMA = "pulp.gpu-first-visible-trace-producer-overhead-raw.v1"
RECEIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-overhead.v1"
METRICS_SCHEMA = "pulp.gpu-first-visible-trace-producer-runtime-metrics.v1"
SESSION_CONFIG_SCHEMA = "pulp.gpu-first-visible-trace-session-config.v1"
COLLECTION_REQUEST_SCHEMA = "pulp.gpu-first-visible-trace-producer-collection-request.v1"
COLLECTION_DRIVER_REQUEST_SCHEMA = "pulp.gpu-first-visible-trace-producer-driver-request.v1"
COLLECTION_DRIVER_RECEIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-driver-receipt.v1"
COLLECTION_RECEIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-collection.v1"
COLLECTION_TRANSCRIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-liveness.v1"
STATE_BUILD_REQUEST_SCHEMA = "pulp.gpu-first-visible-trace-producer-state-build-request.v1"
STATE_BUILD_RECEIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-state-build-receipt.v1"
PRODUCER_REVISION = "8175bd483f5e4ca66989c9ad91a4d9ed5a864bb0"
INPUT_TO_PRESENT_REVISION = "b4ba22f1d700621366afdbc72bb8615336964cd1"
BASELINE_REVISION = "5048ce72dd28d87974550a3feb526de0f44af32c"
INPUT_TO_PRESENT_PATHS = [
    "core/view/platform/mac/window_host_mac.mm",
    "core/view/src/editor_bridge.cpp",
    "core/view/src/view.cpp",
    "core/view/src/widget_bridge.cpp",
    "core/view/src/widget_bridge/bridge_dispatch.cpp",
]
PRODUCER_PACKAGES = {
    "gpu-health-first-visible": {
        "revision": PRODUCER_REVISION,
        "events": ["gpu_health_transition_first_visible"],
    },
    "mac-input-to-present": {
        "revision": INPUT_TO_PRESENT_REVISION,
        "paths": INPUT_TO_PRESENT_PATHS,
        "events": ["gpu_acquire", "gpu_submit", "gpu_present"],
    },
}
RING_BYTES = 128 * 1024 * 1024
STATES = (
    "pre-change-baseline",
    "candidate-compile-out",
    "candidate-compiled-in-idle",
    "candidate-active",
)
CAMPAIGN_ROLES = {"standalone", "headless-constrained", "daw", "forge"}
STATE_TRACING = {
    "pre-change-baseline": (False, False, 0),
    "candidate-compile-out": (False, False, 0),
    "candidate-compiled-in-idle": (True, False, 0),
    "candidate-active": (True, True, RING_BYTES),
}
LIMITS = {
    "candidate-compile-out": {"median_percent": 1.0, "p95_percent": 2.0},
    "candidate-compiled-in-idle": {"median_percent": 2.0, "p95_percent": 5.0},
    "candidate-active": {"median_percent": 5.0, "p95_percent": 10.0},
}
RAW_KEYS = {
    "schema", "version", "state", "producer_revision", "source_revision",
    "baseline_revision", "candidate_revision", "campaign_role", "campaign_id",
    "build_family_id", "product_id", "product_name", "plugin_format",
    "binary_sha256", "machine", "workload", "measurement_driver",
    "trace_session_config", "tracing", "warmups", "measured", "fresh_process",
    "collection", "producer_packages", "state_build_driver",
}
MACHINE_KEYS = {"machine_id", "operating_system", "architecture"}
WORKLOAD_KEYS = {
    "workload_id", "content_sha256", "adapter_sha256", "adapter_revision",
    "adapter_path",
}
DRIVER_KEYS = {"revision", "path", "sha256"}
TRACING_KEYS = {"compiled_in", "session_active", "ring_bytes"}
ARTIFACT_KEYS = {"path", "sha256"}
TRACE_KEYS = {"path", "sha256", "bytes", "format"}
SAMPLE_KEYS = {"sequence", "evidence_id", "duration_ms", "runtime_metrics", "trace"}
METRICS_KEYS = {
    "schema", "version", "state", "sequence", "evidence_id", "host_pid",
    "process_start_identity", "executable_sha256", "audio_thread_tids",
    "started_monotonic_ns", "finished_monotonic_ns", "xrun_count",
    "collection_challenge_nonce", "driver_sha256",
}
SESSION_CONFIG_KEYS = {
    "schema", "version", "ring_bytes", "fill_policy", "categories",
}
COLLECTION_REQUEST_KEYS = RAW_KEYS - {
    "collection", "warmups", "measured", "fresh_process",
}
COLLECTION_DRIVER_REQUEST_KEYS = {
    "schema", "version", "attempt_nonce", "challenge_nonce", "state",
    "binary", "counts", "tracing", "trace_session_config",
    "artifact_directory", "liveness_challenge",
}
COLLECTION_DRIVER_RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "challenge_nonce", "state",
    "outcome", "reason", "binary_sha256", "driver_sha256", "samples",
}
COLLECTION_RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "challenge_nonce", "state",
    "source_revision", "binary_sha256", "driver_sha256", "collector_sha256",
    "driver_artifact_directory", "driver_request", "driver_receipt",
    "liveness_transcript", "handshake_artifacts", "state_build",
}
STATE_BUILD_IDENTITY_KEYS = {
    "campaign_role", "build_family_id", "product_id", "product_name", "plugin_format",
}
STATE_BUILD_REQUEST_KEYS = {
    "schema", "version", "attempt_nonce", "state", "source_revision",
    "candidate_revision", "source_tree", "source_tree_sha256",
    "source_archive_sha256",
    "source_directory", "output_directory", "binary_sha256", "identity",
    "tracing", "driver_sha256",
}
STATE_BUILD_RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "state", "outcome", "reason",
    "source_revision", "candidate_revision", "source_tree", "source_tree_sha256",
    "source_archive_sha256",
    "binary_sha256", "identity", "tracing", "driver_sha256", "build_command",
    "builder_id", "build_started_utc", "build_finished_utc", "toolchain",
    "product_path", "product_sha256",
}
STATE_BUILD_EVIDENCE_KEYS = {
    "source_archive", "build_driver", "build_request", "build_receipt",
    "rebuilt_product", "stdout", "stderr", "toolchain",
}
TOOLCHAIN_KEYS = {"path", "sha256", "version"}
COLLECTION_TRANSCRIPT_KEYS = {
    "schema", "version", "attempt_nonce", "challenge_nonce", "state",
    "binary_sha256", "driver_sha256", "observations",
}
COLLECTION_CHALLENGE_SCHEMA = "pulp.gpu-first-visible-trace-producer-challenge.v1"
COLLECTION_ACK_SCHEMA = "pulp.gpu-first-visible-trace-producer-ack.v1"
COLLECTION_CHALLENGE_KEYS = {
    "schema", "version", "attempt_nonce", "challenge_nonce", "state",
    "section", "sequence", "evidence_id", "host_pid", "audio_thread_tids",
    "started_monotonic_ns", "finished_monotonic_ns", "xrun_count",
}
COLLECTION_OBSERVATION_KEYS = COLLECTION_CHALLENGE_KEYS | {
    "process_start_identity", "executable_sha256",
}
RECEIPT_KEYS = {
    "schema", "version", "generated_utc", "producer_revision",
    "baseline_revision", "candidate_revision", "campaign_role", "campaign_id",
    "build_family_id", "product_id", "product_name", "plugin_format", "binaries",
    "machine", "workload", "measurement_driver", "trace_session_config",
    "trace_processor", "protocol", "raw_artifacts", "summaries",
    "comparisons", "replay_summary", "producer_packages",
    "aggregate_disposition", "verdict", "state_build_driver",
}
SHA256 = re.compile(r"^[0-9a-f]{64}$")
EVIDENCE_ID = re.compile(r"^[0-9a-f]{32}$")
GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")
UTC_TIMESTAMP = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[^ ]+Z$")
TRACING_SENTINEL = b"PULP_TRACING_COMPILED_IN__DO_NOT_SHIP"


class OverheadError(ValueError):
    """The producer-overhead evidence is incomplete or inconsistent."""


def exact_keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise OverheadError(f"{label} has the wrong fields")


def regular_file_bytes(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise OverheadError(f"{label} is not a readable regular file: {path}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise OverheadError(f"{label} is not a regular file: {path}")
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


def sha256_file(path: Path, label: str) -> str:
    return sha256_bytes(regular_file_bytes(path, label))


def live_collection_evidence_id(
    *, attempt_nonce: str, challenge_nonce: str, state: str, section: str,
    sequence: int, host_pid: int, process_start_identity: str,
    executable_sha256: str,
) -> str:
    fields = (
        attempt_nonce, challenge_nonce, state, section, str(sequence), str(host_pid),
        process_start_identity, executable_sha256,
    )
    return hashlib.sha256(
        b"pulp-a3-overhead-live-evidence-v1\0"
        + b"\0".join(field.encode("utf-8") for field in fields)
    ).hexdigest()[:32]


def regular_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(regular_file_bytes(path, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise OverheadError(f"{label} is not valid JSON") from error
    if not isinstance(value, dict):
        raise OverheadError(f"{label} must contain an object")
    return value


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    temporary.chmod(0o600)
    os.replace(temporary, path)


def safe_artifact(root: Path, relative: Any, label: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise OverheadError(f"{label} path is missing")
    lexical = root / relative
    if Path(relative).is_absolute() or ".." in Path(relative).parts or lexical.is_symlink():
        raise OverheadError(f"{label} path is unsafe")
    resolved = lexical.resolve()
    root_resolved = root.resolve()
    if root_resolved not in resolved.parents or not resolved.is_file():
        raise OverheadError(f"{label} artifact is unavailable")
    return resolved


def artifact_path(ref: Any, root: Path, label: str) -> Path:
    exact_keys(ref, ARTIFACT_KEYS, label)
    path = safe_artifact(root, ref["path"], label)
    if not isinstance(ref["sha256"], str) or SHA256.fullmatch(ref["sha256"]) is None:
        raise OverheadError(f"{label} digest is invalid")
    if sha256_file(path, label) != ref["sha256"]:
        raise OverheadError(f"{label} digest differs")
    return path


def artifact_ref(path: Path, evidence_root: Path) -> dict[str, str]:
    resolved = path.resolve()
    root = evidence_root.resolve()
    if root not in resolved.parents:
        raise OverheadError(f"artifact escapes the evidence root: {path}")
    return {
        "path": resolved.relative_to(root).as_posix(),
        "sha256": sha256_file(resolved, "artifact"),
    }


def safe_directory(root: Path, relative: Any, label: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise OverheadError(f"{label} path is missing")
    lexical = root / relative
    if Path(relative).is_absolute() or ".." in Path(relative).parts or lexical.is_symlink():
        raise OverheadError(f"{label} path is unsafe")
    resolved = lexical.resolve()
    if root.resolve() not in resolved.parents or not resolved.is_dir():
        raise OverheadError(f"{label} directory is unavailable")
    return resolved


def projected_driver_ref(
    ref: Any, *, driver_root: Path, evidence_root: Path, label: str,
) -> dict[str, Any]:
    if (
        not isinstance(ref, dict)
        or (set(ref) != ARTIFACT_KEYS and set(ref) != TRACE_KEYS)
    ):
        raise OverheadError(f"{label} has the wrong fields")
    relative = ref.get("path")
    if not isinstance(relative, str) or not relative:
        raise OverheadError(f"{label} path is missing")
    path = safe_artifact(driver_root, relative, label)
    digest = sha256_file(path, label)
    if ref.get("sha256") != digest:
        raise OverheadError(f"{label} digest differs")
    projected: dict[str, Any] = artifact_ref(path, evidence_root)
    if set(ref) == TRACE_KEYS:
        if (
            not isinstance(ref["bytes"], int) or isinstance(ref["bytes"], bool)
            or ref["bytes"] != path.stat().st_size
            or ref["format"] not in {"chrome-json", "perfetto-proto"}
        ):
            raise OverheadError(f"{label} trace metadata is invalid")
        projected.update({"bytes": ref["bytes"], "format": ref["format"]})
    return projected


def state_build_identity(payload: dict[str, Any]) -> dict[str, str]:
    return {
        field: payload[field]
        for field in (
            "campaign_role", "build_family_id", "product_id", "product_name",
            "plugin_format",
        )
    }


def source_archive_tree_sha256(path: Path, label: str) -> str:
    entries: list[tuple[str, bytes, int]] = []
    total_bytes = 0
    try:
        with tarfile.open(path, "r:") as archive:
            for member in archive.getmembers():
                member_path = Path(member.name)
                if (
                    member.name.startswith("/") or ".." in member_path.parts
                    or member.issym() or member.islnk()
                    or not (member.isdir() or member.isfile())
                ):
                    raise OverheadError(f"{label} contains an unsafe member")
                if member.isdir():
                    continue
                total_bytes += member.size
                if member.size > 512 * 1024 * 1024 or total_bytes > 4 * 1024 * 1024 * 1024:
                    raise OverheadError(f"{label} exceeds its bounded source size")
                handle = archive.extractfile(member)
                if handle is None:
                    raise OverheadError(f"{label} contains an unreadable member")
                mode = stat.S_IMODE(member.mode)
                entries.append((
                    member_path.as_posix(), handle.read(),
                    0o755 if mode & 0o111 else 0o644,
                ))
    except (OSError, tarfile.TarError) as error:
        raise OverheadError(f"{label} is not a readable source archive") from error
    if not entries:
        raise OverheadError(f"{label} is empty")
    return role_support.directory_tree_digest(sorted(entries))


def validate_binary_compile_config(path: Path, compiled_in: bool, label: str) -> None:
    sentinel_count = regular_file_bytes(path, label).count(TRACING_SENTINEL)
    if compiled_in and sentinel_count != 1:
        raise OverheadError(f"{label} does not prove one compiled-in tracing sentinel")
    if not compiled_in and sentinel_count != 0:
        raise OverheadError(f"{label} contains a compiled-in tracing sentinel")


def validate_state_build(
    evidence: Any, *, payload: dict[str, Any], evidence_root: Path, label: str,
) -> None:
    exact_keys(evidence, STATE_BUILD_EVIDENCE_KEYS, f"{label}.state_build")
    source_archive = artifact_path(
        evidence["source_archive"], evidence_root,
        f"{label}.state_build source archive",
    )
    build_driver = artifact_path(
        evidence["build_driver"], evidence_root,
        f"{label}.state_build driver",
    )
    build_request_path = artifact_path(
        evidence["build_request"], evidence_root,
        f"{label}.state_build request",
    )
    build_receipt_path = artifact_path(
        evidence["build_receipt"], evidence_root,
        f"{label}.state_build receipt",
    )
    rebuilt_product = artifact_path(
        evidence["rebuilt_product"], evidence_root,
        f"{label}.state_build rebuilt product",
    )
    artifact_path(evidence["stdout"], evidence_root, f"{label}.state_build stdout")
    artifact_path(evidence["stderr"], evidence_root, f"{label}.state_build stderr")
    request = regular_json(build_request_path, f"{label}.state_build request")
    receipt = regular_json(build_receipt_path, f"{label}.state_build receipt")
    exact_keys(request, STATE_BUILD_REQUEST_KEYS, f"{label}.state_build request")
    exact_keys(receipt, STATE_BUILD_RECEIPT_KEYS, f"{label}.state_build receipt")
    driver = payload["state_build_driver"]
    identity = state_build_identity(payload)
    common = (
        request["attempt_nonce"] == receipt["attempt_nonce"]
        and request["state"] == receipt["state"] == payload["state"]
        and request["source_revision"] == receipt["source_revision"]
        == payload["source_revision"]
        and request["candidate_revision"] == receipt["candidate_revision"]
        == payload["candidate_revision"]
        and request["source_tree"] == receipt["source_tree"]
        and request["source_tree_sha256"] == receipt["source_tree_sha256"]
        and request["source_archive_sha256"] == receipt["source_archive_sha256"]
        and request["binary_sha256"] == receipt["binary_sha256"]
        == payload["binary_sha256"]
        and request["identity"] == receipt["identity"] == identity
        and request["tracing"] == receipt["tracing"] == payload["tracing"]
        and request["driver_sha256"] == receipt["driver_sha256"]
        == driver["sha256"]
    )
    if (
        request["schema"] != STATE_BUILD_REQUEST_SCHEMA
        or request["version"] != 1
        or receipt["schema"] != STATE_BUILD_RECEIPT_SCHEMA
        or receipt["version"] != 1
        or not common
        or not isinstance(request["attempt_nonce"], str)
        or not request["attempt_nonce"]
        or not isinstance(request["source_directory"], str)
        or not Path(request["source_directory"]).is_absolute()
        or not isinstance(request["output_directory"], str)
        or not Path(request["output_directory"]).is_absolute()
        or not isinstance(request["source_tree"], str)
        or GIT_REVISION.fullmatch(request["source_tree"]) is None
        or not isinstance(request["source_tree_sha256"], str)
        or SHA256.fullmatch(request["source_tree_sha256"]) is None
        or sha256_file(source_archive, f"{label}.state_build source archive")
        != request["source_archive_sha256"]
        or source_archive_tree_sha256(
            source_archive, f"{label}.state_build source archive",
        ) != request["source_tree_sha256"]
        or sha256_file(build_driver, f"{label}.state_build driver") != driver["sha256"]
        or sha256_file(rebuilt_product, f"{label}.state_build rebuilt product")
        != payload["binary_sha256"]
        or receipt["outcome"] != "pass"
        or receipt["reason"] is not None
        or receipt["product_sha256"] != payload["binary_sha256"]
        or not isinstance(receipt["product_path"], str)
        or not receipt["product_path"]
        or Path(receipt["product_path"]).is_absolute()
        or ".." in Path(receipt["product_path"]).parts
        or not isinstance(receipt["builder_id"], str)
        or not receipt["builder_id"]
        or not isinstance(receipt["build_command"], list)
        or not 1 <= len(receipt["build_command"]) <= 64
        or any(
            not isinstance(item, str) or not item for item in receipt["build_command"]
        )
        or UTC_TIMESTAMP.fullmatch(receipt["build_started_utc"]) is None
        or UTC_TIMESTAMP.fullmatch(receipt["build_finished_utc"]) is None
        or receipt["build_finished_utc"] < receipt["build_started_utc"]
    ):
        raise OverheadError(f"{label}.state_build is not source/config/product bound")
    toolchain = receipt["toolchain"]
    toolchain_refs = evidence["toolchain"]
    if (
        not isinstance(toolchain, list) or not 1 <= len(toolchain) <= 16
        or not isinstance(toolchain_refs, list) or len(toolchain_refs) != len(toolchain)
    ):
        raise OverheadError(f"{label}.state_build lacks bounded toolchain provenance")
    observed_paths: set[str] = set()
    for index, (tool, ref) in enumerate(zip(toolchain, toolchain_refs, strict=True)):
        exact_keys(tool, TOOLCHAIN_KEYS, f"{label}.state_build toolchain[{index}]")
        tool_snapshot = artifact_path(
            ref, evidence_root, f"{label}.state_build toolchain[{index}]",
        )
        if (
            not isinstance(tool["path"], str) or not Path(tool["path"]).is_absolute()
            or tool["path"] in observed_paths
            or not isinstance(tool["sha256"], str)
            or SHA256.fullmatch(tool["sha256"]) is None
            or sha256_file(tool_snapshot, f"{label}.state_build toolchain[{index}]")
            != tool["sha256"]
            or not isinstance(tool["version"], str) or not tool["version"]
        ):
            raise OverheadError(f"{label}.state_build toolchain[{index}] is invalid")
        observed_paths.add(tool["path"])
    if receipt["build_command"][0] != toolchain[0]["path"]:
        raise OverheadError(f"{label}.state_build command is not toolchain bound")
    validate_binary_compile_config(
        rebuilt_product, bool(payload["tracing"]["compiled_in"]),
        f"{label}.state_build rebuilt product",
    )


def validate_collection(
    payload: dict[str, Any], *, evidence_root: Path, label: str,
    allow_fixture_collection: bool,
) -> tuple[str, dict[tuple[str, int], dict[str, Any]]]:
    collection_ref = payload["collection"]
    if collection_ref is None:
        if allow_fixture_collection:
            return "0" * 32, {}
        raise OverheadError(f"{label} lacks a live collector receipt")
    collection_path = artifact_path(
        collection_ref, evidence_root, f"{label}.collection",
    )
    collection = regular_json(collection_path, f"{label}.collection")
    exact_keys(collection, COLLECTION_RECEIPT_KEYS, f"{label}.collection")
    if (
        collection["schema"] != COLLECTION_RECEIPT_SCHEMA
        or collection["version"] != 1
        or collection["state"] != payload["state"]
        or collection["source_revision"] != payload["source_revision"]
        or collection["binary_sha256"] != payload["binary_sha256"]
        or collection["driver_sha256"] != payload["measurement_driver"]["sha256"]
        or collection["collector_sha256"]
        != sha256_file(Path(__file__).resolve(), "A3 overhead collector")
        or not isinstance(collection["attempt_nonce"], str)
        or not collection["attempt_nonce"]
        or not isinstance(collection["challenge_nonce"], str)
        or EVIDENCE_ID.fullmatch(collection["challenge_nonce"]) is None
    ):
        raise OverheadError(f"{label}.collection is not source/product bound")
    validate_state_build(
        collection["state_build"], payload=payload, evidence_root=evidence_root,
        label=label,
    )
    driver_root = safe_directory(
        evidence_root, collection["driver_artifact_directory"],
        f"{label}.collection driver artifacts",
    )
    request_path = artifact_path(
        collection["driver_request"], evidence_root,
        f"{label}.collection driver request",
    )
    receipt_path = artifact_path(
        collection["driver_receipt"], evidence_root,
        f"{label}.collection driver receipt",
    )
    transcript_path = artifact_path(
        collection["liveness_transcript"], evidence_root,
        f"{label}.collection liveness transcript",
    )
    driver_request = regular_json(request_path, f"{label}.collection driver request")
    driver_receipt = regular_json(receipt_path, f"{label}.collection driver receipt")
    transcript = regular_json(transcript_path, f"{label}.collection liveness transcript")
    exact_keys(
        driver_request, COLLECTION_DRIVER_REQUEST_KEYS,
        f"{label}.collection driver request",
    )
    exact_keys(
        driver_receipt, COLLECTION_DRIVER_RECEIPT_KEYS,
        f"{label}.collection driver receipt",
    )
    exact_keys(
        transcript, COLLECTION_TRANSCRIPT_KEYS,
        f"{label}.collection liveness transcript",
    )
    common = (
        driver_request.get("attempt_nonce") == collection["attempt_nonce"]
        and driver_receipt.get("attempt_nonce") == collection["attempt_nonce"]
        and transcript.get("attempt_nonce") == collection["attempt_nonce"]
        and driver_request.get("challenge_nonce") == collection["challenge_nonce"]
        and driver_receipt.get("challenge_nonce") == collection["challenge_nonce"]
        and transcript.get("challenge_nonce") == collection["challenge_nonce"]
        and driver_request.get("state") == payload["state"]
        and driver_receipt.get("state") == payload["state"]
        and transcript.get("state") == payload["state"]
    )
    binary = driver_request.get("binary")
    counts = driver_request.get("counts")
    liveness = driver_request.get("liveness_challenge")
    if (
        driver_request.get("schema") != COLLECTION_DRIVER_REQUEST_SCHEMA
        or driver_request.get("version") != 1
        or not common
        or not isinstance(binary, dict)
        or set(binary) != {"runtime_path", "sha256"}
        or not isinstance(binary.get("runtime_path"), str)
        or not binary["runtime_path"]
        or binary.get("sha256") != payload["binary_sha256"]
        or counts != {"warmups": 5, "measured": 30, "fresh_process": 20}
        or driver_request.get("tracing") != payload["tracing"]
        or driver_request.get("trace_session_config") != payload["trace_session_config"]
        or Path(driver_request.get("artifact_directory", "")).resolve() != driver_root
        or not isinstance(liveness, dict)
        or set(liveness) != {
            "schema", "version", "attempt_nonce", "challenge_nonce", "directory",
            "expected_count",
        }
        or liveness.get("schema") != COLLECTION_CHALLENGE_SCHEMA
        or liveness.get("version") != 1
        or liveness.get("attempt_nonce") != collection["attempt_nonce"]
        or liveness.get("challenge_nonce") != collection["challenge_nonce"]
        or liveness.get("expected_count") != 55
        or Path(liveness.get("directory", "")).resolve()
        != (collection_path.parent / "liveness").resolve()
    ):
        raise OverheadError(f"{label}.collection driver request differs from the raw state")
    if (
        driver_receipt.get("schema") != COLLECTION_DRIVER_RECEIPT_SCHEMA
        or driver_receipt.get("version") != 1
        or driver_receipt.get("outcome") != "pass"
        or driver_receipt.get("reason") is not None
        or driver_receipt.get("binary_sha256") != payload["binary_sha256"]
        or driver_receipt.get("driver_sha256") != payload["measurement_driver"]["sha256"]
        or not common
    ):
        raise OverheadError(f"{label}.collection driver did not return a bound pass")
    if (
        transcript.get("schema") != COLLECTION_TRANSCRIPT_SCHEMA
        or transcript.get("version") != 1
        or transcript.get("binary_sha256") != payload["binary_sha256"]
        or transcript.get("driver_sha256") != payload["measurement_driver"]["sha256"]
        or not common
        or not isinstance(transcript.get("observations"), list)
    ):
        raise OverheadError(f"{label}.collection transcript is not product bound")
    handshake_refs = collection["handshake_artifacts"]
    if not isinstance(handshake_refs, list) or len(handshake_refs) != 110:
        raise OverheadError(f"{label}.collection does not retain 55 challenge/ack pairs")
    handshake_payloads = [
        regular_json(
            artifact_path(ref, evidence_root, f"{label}.handshake[{index}]"),
            f"{label}.handshake[{index}]",
        )
        for index, ref in enumerate(handshake_refs)
    ]
    observations = transcript["observations"]
    if len(observations) != 55:
        raise OverheadError(f"{label}.collection transcript lacks 55 live observations")
    by_key: dict[tuple[str, int], dict[str, Any]] = {}
    expected_sections = (("warmups", 5), ("measured", 30), ("fresh_process", 20))
    for observation in observations:
        exact_keys(observation, COLLECTION_OBSERVATION_KEYS, f"{label} live observation")
        key = (observation["section"], observation["sequence"])
        if (
            observation["schema"] != COLLECTION_ACK_SCHEMA
            or observation["version"] != 1
            or observation["attempt_nonce"] != collection["attempt_nonce"]
            or observation["challenge_nonce"] != collection["challenge_nonce"]
            or observation["state"] != payload["state"]
            or observation["executable_sha256"] != payload["binary_sha256"]
            or key in by_key
        ):
            raise OverheadError(f"{label}.collection has an invalid live observation")
        by_key[key] = observation
    if set(by_key) != {
        (section, sequence) for section, count in expected_sections
        for sequence in range(count)
    }:
        raise OverheadError(f"{label}.collection live observations are incomplete")
    challenges = [item for item in handshake_payloads if item.get("schema") == COLLECTION_CHALLENGE_SCHEMA]
    acks = [item for item in handshake_payloads if item.get("schema") == COLLECTION_ACK_SCHEMA]
    if len(challenges) != 55 or len(acks) != 55:
        raise OverheadError(f"{label}.collection handshake artifacts have the wrong roles")
    for challenge in challenges:
        exact_keys(challenge, COLLECTION_CHALLENGE_KEYS, f"{label} live challenge")
        key = (challenge["section"], challenge["sequence"])
        observation = by_key.get(key)
        if observation is None or any(
            observation[field] != challenge[field] for field in COLLECTION_CHALLENGE_KEYS
            if field != "schema"
        ):
            raise OverheadError(f"{label}.collection challenge differs from its live ack")
    if sorted(acks, key=lambda item: (item["section"], item["sequence"])) != sorted(
        observations, key=lambda item: (item["section"], item["sequence"])
    ):
        raise OverheadError(f"{label}.collection retained ack differs from its transcript")
    samples = driver_receipt.get("samples")
    if not isinstance(samples, dict) or set(samples) != {item[0] for item in expected_sections}:
        raise OverheadError(f"{label}.collection driver sample sections are incomplete")
    for section, count in expected_sections:
        driver_rows = samples[section]
        raw_rows = payload[section]
        if not isinstance(driver_rows, list) or len(driver_rows) != count or len(raw_rows) != count:
            raise OverheadError(f"{label}.collection {section} count differs")
        for sequence, (driver_row, raw_row) in enumerate(zip(driver_rows, raw_rows, strict=True)):
            exact_keys(driver_row, SAMPLE_KEYS, f"{label}.collection {section}[{sequence}]")
            expected = {
                "sequence": driver_row["sequence"],
                "evidence_id": driver_row["evidence_id"],
                "duration_ms": driver_row["duration_ms"],
                "runtime_metrics": projected_driver_ref(
                    driver_row["runtime_metrics"], driver_root=driver_root,
                    evidence_root=evidence_root,
                    label=f"{label}.collection {section}[{sequence}].runtime_metrics",
                ),
                "trace": (
                    projected_driver_ref(
                        driver_row["trace"], driver_root=driver_root,
                        evidence_root=evidence_root,
                        label=f"{label}.collection {section}[{sequence}].trace",
                    ) if driver_row["trace"] is not None else None
                ),
            }
            if raw_row != expected:
                raise OverheadError(f"{label}.{section}[{sequence}] is not collector-derived")
    return collection["challenge_nonce"], by_key


def finite_positive(value: Any) -> bool:
    return (
        isinstance(value, (int, float)) and not isinstance(value, bool)
        and math.isfinite(value) and value > 0
    )


def validate_session_config(ref: Any, evidence_root: Path) -> tuple[Path, str]:
    path = artifact_path(ref, evidence_root, "trace_session_config")
    payload = regular_json(path, "trace_session_config")
    exact_keys(payload, SESSION_CONFIG_KEYS, "trace_session_config")
    if payload != {
        "schema": SESSION_CONFIG_SCHEMA,
        "version": 1,
        "ring_bytes": RING_BYTES,
        "fill_policy": "ring-buffer",
        "categories": ["dsp", "gpu", "metadata", "render"],
    }:
        raise OverheadError("trace_session_config is not the closed 128 MiB session")
    return path, ref["sha256"]


def validate_metrics(
    ref: Any, *, state: str, sequence: int, evidence_id: str,
    binary_sha256: str, evidence_root: Path, duration_ms: float, label: str,
    driver_sha256: str, challenge_nonce: str,
) -> dict[str, Any]:
    path = artifact_path(ref, evidence_root, f"{label}.runtime_metrics")
    metrics = regular_json(path, f"{label}.runtime_metrics")
    exact_keys(metrics, METRICS_KEYS, f"{label}.runtime_metrics")
    tids = metrics["audio_thread_tids"]
    if (
        metrics["schema"] != METRICS_SCHEMA
        or metrics["version"] != 1
        or metrics["state"] != state
        or metrics["sequence"] != sequence
        or metrics["evidence_id"] != evidence_id
        or not isinstance(metrics["host_pid"], int)
        or isinstance(metrics["host_pid"], bool)
        or metrics["host_pid"] <= 0
        or not isinstance(metrics["process_start_identity"], str)
        or not metrics["process_start_identity"]
        or metrics["executable_sha256"] != binary_sha256
        or metrics["driver_sha256"] != driver_sha256
        or metrics["collection_challenge_nonce"] != challenge_nonce
        or not isinstance(tids, list)
        or not tids
        or tids != sorted(set(tids))
        or any(not isinstance(item, int) or isinstance(item, bool) or item <= 0 for item in tids)
        or not isinstance(metrics["started_monotonic_ns"], int)
        or isinstance(metrics["started_monotonic_ns"], bool)
        or not isinstance(metrics["finished_monotonic_ns"], int)
        or isinstance(metrics["finished_monotonic_ns"], bool)
        or metrics["finished_monotonic_ns"] <= metrics["started_monotonic_ns"]
        or metrics["xrun_count"] != 0
    ):
        raise OverheadError(f"{label}.runtime_metrics lacks authentic process/xrun facts")
    derived_duration = round(
        (metrics["finished_monotonic_ns"] - metrics["started_monotonic_ns"]) / 1_000_000,
        6,
    )
    if round(float(duration_ms), 6) != derived_duration:
        raise OverheadError(f"{label}.duration_ms is not derived from runtime metrics")
    return metrics


def validate_trace(
    ref: Any, *, metrics: dict[str, Any], evidence_id: str,
    session_config_sha256: str, evidence_root: Path,
    trace_processor: Path | None, label: str, allow_fixture_chrome_json: bool,
) -> dict[str, Any]:
    exact_keys(ref, TRACE_KEYS, f"{label}.trace")
    path = safe_artifact(evidence_root, ref["path"], f"{label}.trace")
    data = regular_file_bytes(path, f"{label}.trace")
    if (
        not isinstance(ref["sha256"], str)
        or SHA256.fullmatch(ref["sha256"]) is None
        or sha256_bytes(data) != ref["sha256"]
        or not isinstance(ref["bytes"], int)
        or isinstance(ref["bytes"], bool)
        or ref["bytes"] != len(data)
        or not 0 < len(data) <= RING_BYTES
        or ref["format"] not in {"chrome-json", "perfetto-proto"}
    ):
        raise OverheadError(f"{label}.trace lacks bounded immutable bytes")
    request = {
        "evidence_id": evidence_id,
        "host_pid": metrics["host_pid"],
        "process_start_identity": metrics["process_start_identity"],
        "executable_sha256": metrics["executable_sha256"],
        "audio_thread_tids": metrics["audio_thread_tids"],
        "session_config_sha256": session_config_sha256,
        "ring_bytes": RING_BYTES,
        "collection_challenge_nonce": metrics["collection_challenge_nonce"],
    }
    try:
        analysis = trace_replay.analyze_trace(
            path, request, trace_processor,
            allow_fixture_chrome_json=allow_fixture_chrome_json,
        )
    except (OSError, trace_replay.TraceReplayError) as error:
        raise OverheadError(f"{label}.trace replay failed: {error}") from error
    if analysis["trace_format"] != ref["format"]:
        raise OverheadError(f"{label}.trace format differs from replay")
    return analysis


def validate_samples(
    rows: Any, *, count: int, state: str, evidence_root: Path,
    binary_sha256: str, session_config_sha256: str,
    trace_processor: Path | None, label: str, driver_sha256: str,
    challenge_nonce: str, allow_fixture_chrome_json: bool,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    if not isinstance(rows, list) or len(rows) != count:
        raise OverheadError(f"{label} must contain exactly {count} samples")
    active = state == "candidate-active"
    metrics_rows: list[dict[str, Any]] = []
    analyses: list[dict[str, Any]] = []
    evidence_ids: set[str] = set()
    metric_paths: set[str] = set()
    for index, row in enumerate(rows):
        exact_keys(row, SAMPLE_KEYS, f"{label}[{index}]")
        if (
            row["sequence"] != index
            or not isinstance(row["evidence_id"], str)
            or EVIDENCE_ID.fullmatch(row["evidence_id"]) is None
            or row["evidence_id"] in evidence_ids
            or not finite_positive(row["duration_ms"])
        ):
            raise OverheadError(f"{label}[{index}] lacks bounded timing/identity")
        evidence_ids.add(row["evidence_id"])
        metrics_ref = row["runtime_metrics"]
        exact_keys(metrics_ref, ARTIFACT_KEYS, f"{label}[{index}].runtime_metrics")
        if metrics_ref["path"] in metric_paths:
            raise OverheadError(f"{label} reuses a runtime metrics artifact")
        metric_paths.add(metrics_ref["path"])
        metrics = validate_metrics(
            metrics_ref, state=state, sequence=index, evidence_id=row["evidence_id"],
            binary_sha256=binary_sha256, evidence_root=evidence_root,
            duration_ms=float(row["duration_ms"]), label=f"{label}[{index}]",
            driver_sha256=driver_sha256, challenge_nonce=challenge_nonce,
        )
        metrics_rows.append(metrics)
        if active:
            if row["trace"] is None:
                raise OverheadError(f"{label}[{index}] lacks its active trace")
            analyses.append(validate_trace(
                row["trace"], metrics=metrics, evidence_id=row["evidence_id"],
                session_config_sha256=session_config_sha256,
                evidence_root=evidence_root, trace_processor=trace_processor,
                label=f"{label}[{index}]",
                allow_fixture_chrome_json=allow_fixture_chrome_json,
            ))
        elif row["trace"] is not None:
            raise OverheadError(f"{label}[{index}] inactive state claims a trace")
    if label.endswith("fresh_process"):
        processes = {
            (item["host_pid"], item["process_start_identity"])
            for item in metrics_rows
        }
        if len(processes) != count:
            raise OverheadError(f"{label} does not contain {count} unique process starts")
    return metrics_rows, analyses


def validate_raw(
    payload: dict[str, Any], *, state: str, evidence_root: Path,
    trace_processor: Path | None, label: str,
    allow_fixture_collection: bool, allow_fixture_chrome_json: bool,
) -> dict[str, Any]:
    exact_keys(payload, RAW_KEYS, label)
    exact_keys(payload["machine"], MACHINE_KEYS, f"{label}.machine")
    exact_keys(payload["workload"], WORKLOAD_KEYS, f"{label}.workload")
    exact_keys(payload["measurement_driver"], DRIVER_KEYS, f"{label}.measurement_driver")
    exact_keys(payload["state_build_driver"], DRIVER_KEYS, f"{label}.state_build_driver")
    exact_keys(payload["tracing"], TRACING_KEYS, f"{label}.tracing")
    compiled, active, ring = STATE_TRACING[state]
    if (
        payload["schema"] != RAW_SCHEMA
        or payload["version"] != 1
        or payload["state"] != state
        or payload["producer_revision"] != PRODUCER_REVISION
        or payload["producer_packages"] != PRODUCER_PACKAGES
        or not isinstance(payload["source_revision"], str)
        or GIT_REVISION.fullmatch(payload["source_revision"]) is None
        or payload["baseline_revision"] != BASELINE_REVISION
        or not isinstance(payload["candidate_revision"], str)
        or GIT_REVISION.fullmatch(payload["candidate_revision"]) is None
        or payload["baseline_revision"] == payload["candidate_revision"]
        or payload["source_revision"] != (
            payload["baseline_revision"] if state == "pre-change-baseline"
            else payload["candidate_revision"]
        )
        or payload["campaign_role"] not in CAMPAIGN_ROLES
        or any(
            not isinstance(payload[field], str) or not payload[field]
            for field in (
                "campaign_id", "build_family_id", "product_id", "product_name",
                "plugin_format",
            )
        )
        or not isinstance(payload["binary_sha256"], str)
        or SHA256.fullmatch(payload["binary_sha256"]) is None
        or payload["tracing"] != {
            "compiled_in": compiled, "session_active": active, "ring_bytes": ring,
        }
    ):
        raise OverheadError(f"{label} has an invalid state/source/build contract")
    machine = payload["machine"]
    if any(not isinstance(machine[key], str) or not machine[key] for key in MACHINE_KEYS):
        raise OverheadError(f"{label}.machine is incomplete")
    workload = payload["workload"]
    if (
        not isinstance(workload["workload_id"], str) or not workload["workload_id"]
        or any(
            not isinstance(workload[key], str) or SHA256.fullmatch(workload[key]) is None
            for key in ("content_sha256", "adapter_sha256")
        )
        or workload["adapter_revision"] != payload["candidate_revision"]
        or not isinstance(workload["adapter_path"], str)
        or not workload["adapter_path"]
        or Path(workload["adapter_path"]).is_absolute()
        or ".." in Path(workload["adapter_path"]).parts
    ):
        raise OverheadError(f"{label}.workload is invalid")
    driver = payload["measurement_driver"]
    if (
        driver["revision"] != payload["candidate_revision"]
        or not isinstance(driver["path"], str) or not driver["path"]
        or Path(driver["path"]).is_absolute() or ".." in Path(driver["path"]).parts
        or not isinstance(driver["sha256"], str) or SHA256.fullmatch(driver["sha256"]) is None
    ):
        raise OverheadError(f"{label}.measurement_driver is not source-bound")
    state_build_driver = payload["state_build_driver"]
    if (
        state_build_driver["revision"] != payload["candidate_revision"]
        or not isinstance(state_build_driver["path"], str)
        or not state_build_driver["path"]
        or Path(state_build_driver["path"]).is_absolute()
        or ".." in Path(state_build_driver["path"]).parts
        or not isinstance(state_build_driver["sha256"], str)
        or SHA256.fullmatch(state_build_driver["sha256"]) is None
    ):
        raise OverheadError(f"{label}.state_build_driver is not source-bound")
    challenge_nonce, live_observations = validate_collection(
        payload, evidence_root=evidence_root, label=label,
        allow_fixture_collection=allow_fixture_collection,
    )
    _, session_digest = validate_session_config(payload["trace_session_config"], evidence_root)
    metrics: dict[str, list[dict[str, Any]]] = {}
    analyses: dict[str, list[dict[str, Any]]] = {}
    for section, count in (("warmups", 5), ("measured", 30), ("fresh_process", 20)):
        metrics[section], analyses[section] = validate_samples(
            payload[section], count=count, state=state, evidence_root=evidence_root,
            binary_sha256=payload["binary_sha256"],
            session_config_sha256=session_digest,
            trace_processor=trace_processor, label=f"{label}.{section}",
            driver_sha256=driver["sha256"], challenge_nonce=challenge_nonce,
            allow_fixture_chrome_json=allow_fixture_chrome_json,
        )
        if live_observations:
            for sequence, observed_metrics in enumerate(metrics[section]):
                observation = live_observations[(section, sequence)]
                expected = {
                    "sequence": observation["sequence"],
                    "evidence_id": observation["evidence_id"],
                    "host_pid": observation["host_pid"],
                    "process_start_identity": observation["process_start_identity"],
                    "executable_sha256": observation["executable_sha256"],
                    "audio_thread_tids": observation["audio_thread_tids"],
                    "started_monotonic_ns": observation["started_monotonic_ns"],
                    "finished_monotonic_ns": observation["finished_monotonic_ns"],
                    "xrun_count": observation["xrun_count"],
                    "collection_challenge_nonce": observation["challenge_nonce"],
                    "driver_sha256": driver["sha256"],
                }
                if any(observed_metrics[key] != value for key, value in expected.items()):
                    raise OverheadError(
                        f"{label}.{section}[{sequence}] differs from its live observation"
                    )
    return {"payload": payload, "metrics": metrics, "analyses": analyses}


def percentile95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[math.ceil(0.95 * len(ordered)) - 1]


def summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    values = [float(row["duration_ms"]) for row in rows]
    return {
        "count": len(values),
        "median_ms": round(statistics.median(values), 6),
        "p95_ms": round(percentile95(values), 6),
    }


def delta_percent(candidate: float, baseline: float) -> float:
    return round((candidate - baseline) * 100.0 / baseline, 6)


def comparison(
    baseline: dict[str, Any], candidate: dict[str, Any], limits: dict[str, float],
) -> dict[str, Any]:
    median_delta = delta_percent(candidate["median_ms"], baseline["median_ms"])
    p95_delta = delta_percent(candidate["p95_ms"], baseline["p95_ms"])
    return {
        "median_delta_percent": median_delta,
        "p95_delta_percent": p95_delta,
        "median_limit_percent": limits["median_percent"],
        "p95_limit_percent": limits["p95_percent"],
        "pass": (
            median_delta <= limits["median_percent"]
            and p95_delta <= limits["p95_percent"]
        ),
    }


def build_receipt(
    raw_paths: dict[str, Path], *, evidence_root: Path, generated_utc: str,
    trace_processor_path: Path | None = None,
    allow_fixture_collection: bool = False,
    allow_fixture_chrome_json: bool = False,
) -> dict[str, Any]:
    if set(raw_paths) != set(STATES):
        raise OverheadError("all four producer-overhead states are required")
    if UTC_TIMESTAMP.fullmatch(generated_utc) is None:
        raise OverheadError("generated_utc is not a UTC timestamp")
    if trace_processor_path is not None:
        try:
            relative_processor = trace_processor_path.resolve().relative_to(
                evidence_root.resolve()
            ).as_posix()
        except ValueError as error:
            raise OverheadError("trace_processor escapes the evidence root") from error
        trace_processor_path = safe_artifact(
            evidence_root, relative_processor, "trace_processor",
        )
        if not os.access(trace_processor_path, os.X_OK):
            raise OverheadError("trace_processor is not executable")
    raw = {
        state: validate_raw(
            regular_json(raw_paths[state], f"{state} raw evidence"),
            state=state, evidence_root=evidence_root,
            trace_processor=trace_processor_path, label=state,
            allow_fixture_collection=allow_fixture_collection,
            allow_fixture_chrome_json=allow_fixture_chrome_json,
        )
        for state in STATES
    }
    payloads = {state: raw[state]["payload"] for state in STATES}
    common_fields = (
        "producer_revision", "baseline_revision", "candidate_revision",
        "campaign_role", "campaign_id", "build_family_id", "product_id",
        "product_name", "plugin_format", "machine", "workload",
        "measurement_driver", "trace_session_config",
        "producer_packages", "state_build_driver",
    )
    baseline = payloads["pre-change-baseline"]
    for state in STATES[1:]:
        if any(payloads[state][field] != baseline[field] for field in common_fields):
            raise OverheadError(f"{state} does not use the same product/host/workload family")
    if (
        payloads["candidate-compiled-in-idle"]["binary_sha256"]
        != payloads["candidate-active"]["binary_sha256"]
    ):
        raise OverheadError("compiled-in idle and active states must use the same product binary")
    if (
        payloads["candidate-compile-out"]["binary_sha256"]
        == payloads["candidate-compiled-in-idle"]["binary_sha256"]
    ):
        raise OverheadError("compile-out and compiled-in states do not identify distinct builds")
    all_ids = [
        row["evidence_id"] for state in STATES for section in (
            "warmups", "measured", "fresh_process",
        ) for row in payloads[state][section]
    ]
    if len(set(all_ids)) != len(all_ids):
        raise OverheadError("producer-overhead evidence IDs are not globally unique")
    fresh_processes = [
        (metrics["host_pid"], metrics["process_start_identity"])
        for state in STATES for metrics in raw[state]["metrics"]["fresh_process"]
    ]
    if len(set(fresh_processes)) != len(fresh_processes):
        raise OverheadError("fresh-process identities are reused across overhead states")
    summaries = {
        state: {
            "measured": summary(payloads[state]["measured"]),
            "fresh_process": summary(payloads[state]["fresh_process"]),
        }
        for state in STATES
    }
    comparisons: dict[str, Any] = {}
    passed = True
    for state in STATES[1:]:
        state_comparison = {
            section: comparison(
                summaries["pre-change-baseline"][section],
                summaries[state][section], LIMITS[state],
            )
            for section in ("measured", "fresh_process")
        }
        state_comparison["pass"] = all(
            state_comparison[section]["pass"]
            for section in ("measured", "fresh_process")
        )
        comparisons[state] = state_comparison
        passed = passed and state_comparison["pass"]
    active_analyses = [
        analysis for section in ("warmups", "measured", "fresh_process")
        for analysis in raw["candidate-active"]["analyses"][section]
    ]
    return {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "generated_utc": generated_utc,
        "producer_revision": PRODUCER_REVISION,
        "producer_packages": PRODUCER_PACKAGES,
        "baseline_revision": baseline["baseline_revision"],
        "candidate_revision": baseline["candidate_revision"],
        "campaign_role": baseline["campaign_role"],
        "campaign_id": baseline["campaign_id"],
        "build_family_id": baseline["build_family_id"],
        "product_id": baseline["product_id"],
        "product_name": baseline["product_name"],
        "plugin_format": baseline["plugin_format"],
        "binaries": {state: payloads[state]["binary_sha256"] for state in STATES},
        "machine": baseline["machine"],
        "workload": baseline["workload"],
        "measurement_driver": baseline["measurement_driver"],
        "state_build_driver": baseline["state_build_driver"],
        "trace_session_config": baseline["trace_session_config"],
        "trace_processor": (
            artifact_ref(trace_processor_path, evidence_root)
            if trace_processor_path is not None else None
        ),
        "protocol": {
            "warmups": 5,
            "measured_trials": 30,
            "fresh_process_trials": 20,
            "ring_bytes": RING_BYTES,
            "xrun_limit": 0,
            "audio_thread_trace_event_limit": 0,
            "comparison_baseline": "pre-change-baseline",
        },
        "raw_artifacts": {
            state: artifact_ref(raw_paths[state], evidence_root) for state in STATES
        },
        "summaries": summaries,
        "comparisons": comparisons,
        "replay_summary": {
            "active_trace_count": len(active_analyses),
            "trace_formats": sorted({item["trace_format"] for item in active_analyses}),
            "producer_events": sum(item["producer_events"] for item in active_analyses),
            "foreign_producer_events": sum(
                item["foreign_producer_events"] for item in active_analyses
            ),
            "xrun_events": sum(item["xrun_events"] for item in active_analyses),
            "audio_thread_producer_events": sum(
                item["audio_thread_producer_events"] for item in active_analyses
            ),
            "input_to_present_events": {
                name: sum(
                    item["input_to_present_events"][name]
                    for item in active_analyses
                )
                for name in ("gpu_acquire", "gpu_submit", "gpu_present")
            },
            "audio_thread_input_to_present_events": sum(
                item["audio_thread_input_to_present_events"]
                for item in active_analyses
            ),
        },
        "aggregate_disposition": {
            "scope": "aggregate-input-to-first-visible-producer-overhead",
            "compiled_out": "measured-against-exact-pre-producer-baseline",
            "compiled_in_idle": "measured-no-session",
            "active_128_mib_ring": "measured-lossless-replayed",
            "xrun_disposition": "pass-zero-runtime-and-trace-xruns",
            "audio_thread_disposition": "pass-zero-producer-events",
        },
        "verdict": "pass" if passed else "fail",
    }


def validate_receipt(
    receipt: dict[str, Any], evidence_root: Path, *, require_pass: bool = True,
    allow_fixture_collection: bool = False,
    allow_fixture_chrome_json: bool = False,
) -> None:
    exact_keys(receipt, RECEIPT_KEYS, "producer-overhead receipt")
    if receipt["schema"] != RECEIPT_SCHEMA or receipt["version"] != 1:
        raise OverheadError("producer-overhead receipt has the wrong protocol")
    refs = receipt["raw_artifacts"]
    exact_keys(refs, set(STATES), "producer-overhead raw artifacts")
    raw_paths: dict[str, Path] = {}
    for state in STATES:
        raw_paths[state] = artifact_path(
            refs[state], evidence_root, f"raw_artifacts.{state}",
        )
    processor_ref = receipt["trace_processor"]
    processor = (
        artifact_path(processor_ref, evidence_root, "trace_processor")
        if processor_ref is not None else None
    )
    if processor is not None and not os.access(processor, os.X_OK):
        raise OverheadError("trace_processor is not executable")
    expected = build_receipt(
        raw_paths, evidence_root=evidence_root,
        generated_utc=receipt["generated_utc"], trace_processor_path=processor,
        allow_fixture_collection=allow_fixture_collection,
        allow_fixture_chrome_json=allow_fixture_chrome_json,
    )
    if receipt != expected:
        raise OverheadError("producer-overhead receipt is not derived from its raw evidence")
    if require_pass and receipt["verdict"] != "pass":
        raise OverheadError("producer-overhead ceilings failed")


class CollectionLivenessMonitor:
    """Challenge every overhead sample while its exact product process is live."""

    def __init__(
        self, *, root: Path, attempt_nonce: str, state: str,
        binary: Path, binary_sha256: str,
    ) -> None:
        self.root = root
        self.root.mkdir()
        self.attempt_nonce = attempt_nonce
        self.state = state
        self.binary = binary.resolve()
        self.binary_sha256 = binary_sha256
        self.challenge_nonce = os.urandom(16).hex()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.error: BaseException | None = None
        self.observations: list[dict[str, Any]] = []
        self.artifacts: list[Path] = []

    @staticmethod
    def expected() -> list[tuple[str, int]]:
        return [
            (section, sequence)
            for section, count in (("warmups", 5), ("measured", 30), ("fresh_process", 20))
            for sequence in range(count)
        ]

    def contract(self) -> dict[str, Any]:
        return {
            "schema": COLLECTION_CHALLENGE_SCHEMA,
            "version": 1,
            "attempt_nonce": self.attempt_nonce,
            "challenge_nonce": self.challenge_nonce,
            "directory": str(self.root),
            "expected_count": 55,
        }

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        try:
            for index, (section, sequence) in enumerate(self.expected()):
                challenge_path = self.root / f"challenge-{index:02}.json"
                while not challenge_path.is_file():
                    if self.stop_event.wait(0.01):
                        return
                challenge = regular_json(
                    challenge_path, f"collection challenge {section}[{sequence}]",
                )
                exact_keys(
                    challenge, COLLECTION_CHALLENGE_KEYS,
                    f"collection challenge {section}[{sequence}]",
                )
                tids = challenge["audio_thread_tids"]
                if (
                    challenge["schema"] != COLLECTION_CHALLENGE_SCHEMA
                    or challenge["version"] != 1
                    or challenge["attempt_nonce"] != self.attempt_nonce
                    or challenge["challenge_nonce"] != self.challenge_nonce
                    or challenge["state"] != self.state
                    or challenge["section"] != section
                    or challenge["sequence"] != sequence
                    or not isinstance(challenge["evidence_id"], str)
                    or EVIDENCE_ID.fullmatch(challenge["evidence_id"]) is None
                    or not isinstance(challenge["host_pid"], int)
                    or isinstance(challenge["host_pid"], bool)
                    or challenge["host_pid"] <= 1
                    or not isinstance(tids, list) or not tids
                    or tids != sorted(set(tids))
                    or any(
                        not isinstance(tid, int) or isinstance(tid, bool) or tid <= 0
                        for tid in tids
                    )
                    or not isinstance(challenge["started_monotonic_ns"], int)
                    or isinstance(challenge["started_monotonic_ns"], bool)
                    or not isinstance(challenge["finished_monotonic_ns"], int)
                    or isinstance(challenge["finished_monotonic_ns"], bool)
                    or challenge["finished_monotonic_ns"]
                    <= challenge["started_monotonic_ns"]
                    or challenge["xrun_count"] != 0
                ):
                    raise OverheadError(
                        f"collection challenge {section}[{sequence}] is invalid: {challenge}"
                    )
                pid = challenge["host_pid"]
                executable = role_support.live_process_executable(pid)
                if (
                    executable != self.binary
                    or sha256_file(executable, "live overhead product")
                    != self.binary_sha256
                ):
                    raise OverheadError(
                        f"collection challenge {section}[{sequence}] names the wrong live product"
                    )
                process_start_identity = role_support.live_process_start_identity(pid)
                expected_evidence_id = live_collection_evidence_id(
                    attempt_nonce=self.attempt_nonce,
                    challenge_nonce=self.challenge_nonce,
                    state=self.state,
                    section=section,
                    sequence=sequence,
                    host_pid=pid,
                    process_start_identity=process_start_identity,
                    executable_sha256=self.binary_sha256,
                )
                if challenge["evidence_id"] != expected_evidence_id:
                    raise OverheadError(
                        f"collection challenge {section}[{sequence}] evidence ID is not "
                        "derived from the challenged process lifetime"
                    )
                observation = {
                    **challenge,
                    "schema": COLLECTION_ACK_SCHEMA,
                    "process_start_identity": process_start_identity,
                    "executable_sha256": self.binary_sha256,
                }
                ack_path = self.root / f"ack-{index:02}.json"
                atomic_json(ack_path, observation)
                self.observations.append(observation)
                self.artifacts.extend((challenge_path, ack_path))
        except BaseException as error:  # surfaced by finish()
            self.error = error

    def finish(self) -> tuple[list[dict[str, Any]], list[Path]]:
        self.stop_event.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            raise OverheadError("collection liveness monitor did not stop")
        if self.error is not None:
            if isinstance(self.error, OverheadError):
                raise self.error
            raise OverheadError(f"collection liveness monitor failed: {self.error}")
        return self.observations, self.artifacts


def validate_collection_request(
    request: dict[str, Any], *, evidence_root: Path, source_root: Path,
    binary: Path, driver: Path, build_driver: Path,
) -> None:
    exact_keys(request, COLLECTION_REQUEST_KEYS, "collection request")
    state = request.get("state")
    driver_record = request.get("measurement_driver")
    build_driver_record = request.get("state_build_driver")
    workload = request.get("workload")
    if (
        request.get("schema") != COLLECTION_REQUEST_SCHEMA
        or request.get("version") != 1
        or state not in STATES
        or request.get("producer_revision") != PRODUCER_REVISION
        or not isinstance(request.get("source_revision"), str)
        or GIT_REVISION.fullmatch(request["source_revision"]) is None
        or request.get("baseline_revision") != BASELINE_REVISION
        or not isinstance(request.get("candidate_revision"), str)
        or GIT_REVISION.fullmatch(request["candidate_revision"]) is None
        or request.get("source_revision") != (
            BASELINE_REVISION if state == "pre-change-baseline"
            else request["candidate_revision"]
        )
        or request.get("campaign_role") not in CAMPAIGN_ROLES
        or not isinstance(request.get("binary_sha256"), str)
        or SHA256.fullmatch(request["binary_sha256"]) is None
        or request.get("tracing") != {
            "compiled_in": STATE_TRACING[state][0],
            "session_active": STATE_TRACING[state][1],
            "ring_bytes": STATE_TRACING[state][2],
        }
        or not isinstance(driver_record, dict)
        or set(driver_record) != DRIVER_KEYS
        or driver_record.get("revision") != request.get("candidate_revision")
        or not isinstance(driver_record.get("path"), str)
        or Path(driver_record["path"]).is_absolute()
        or ".." in Path(driver_record["path"]).parts
        or driver_record.get("sha256") != sha256_file(driver, "collection driver")
        or not isinstance(build_driver_record, dict)
        or set(build_driver_record) != DRIVER_KEYS
        or build_driver_record.get("revision") != request.get("candidate_revision")
        or not isinstance(build_driver_record.get("path"), str)
        or not build_driver_record["path"]
        or Path(build_driver_record["path"]).is_absolute()
        or ".." in Path(build_driver_record["path"]).parts
        or build_driver_record.get("sha256")
        != sha256_file(build_driver, "state build driver")
        or not isinstance(workload, dict) or set(workload) != WORKLOAD_KEYS
        or workload.get("adapter_revision") != request.get("candidate_revision")
        or workload.get("adapter_path") != driver_record.get("path")
        or workload.get("adapter_sha256") != driver_record.get("sha256")
        or sha256_file(binary, "collection product") != request.get("binary_sha256")
    ):
        raise OverheadError("collection request has an invalid state/source/product contract")
    if not binary.is_file() or binary.is_symlink() or not os.access(binary, os.X_OK):
        raise OverheadError("collection binary is not a regular executable")
    try:
        role_support.validate_source_root(
            source_root, request["candidate_revision"], "A3 collection source",
        )
        expected_driver = (source_root / driver_record["path"]).resolve()
        if driver.resolve() != expected_driver:
            raise OverheadError("collection driver is not its source-bound path")
        expected_digest = role_support.sha256_bytes(role_support.git_file_bytes(
            source_root, request["candidate_revision"], driver_record["path"],
            "overhead collection driver",
        ))
        expected_build_driver = (source_root / build_driver_record["path"]).resolve()
        expected_build_driver_digest = role_support.sha256_bytes(
            role_support.git_file_bytes(
                source_root, request["candidate_revision"],
                build_driver_record["path"], "overhead state build driver",
            )
        )
        adapter = workload
        expected_adapter = (source_root / adapter["adapter_path"]).resolve()
        adapter_digest = role_support.sha256_bytes(role_support.git_file_bytes(
            source_root, request["candidate_revision"], adapter["adapter_path"],
            "overhead workload adapter",
        ))
    except role_support.ProducerError as error:
        raise OverheadError(str(error)) from error
    if expected_digest != driver_record["sha256"]:
        raise OverheadError("collection driver differs from the exact candidate revision")
    if (
        expected_build_driver != build_driver.resolve()
        or expected_build_driver_digest != build_driver_record["sha256"]
    ):
        raise OverheadError(
            "state build driver differs from the exact candidate revision"
        )
    if (
        adapter["adapter_revision"] != request["candidate_revision"]
        or expected_adapter != driver.resolve()
        or adapter_digest != adapter["adapter_sha256"]
        or adapter_digest != driver_record["sha256"]
    ):
        raise OverheadError(
            "collection workload adapter is not the exact source-bound measurement driver"
        )
    validate_session_config(request["trace_session_config"], evidence_root)


def run_state_source_build(
    *, request: dict[str, Any], evidence_root: Path, source_root: Path,
    binary: Path, driver: Path, build_driver: Path, collection_root: Path,
    attempt_nonce: str,
) -> dict[str, Any]:
    """Independently rebuild and byte-compare the exact state product."""
    if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
        raise OverheadError("state source proof requires the macOS sandbox")
    evidence_resolved = evidence_root.resolve()
    source_resolved = source_root.resolve()
    if (
        evidence_resolved == source_resolved
        or evidence_resolved in source_resolved.parents
        or source_resolved in evidence_resolved.parents
    ):
        raise OverheadError("state source root overlaps the evidence root")
    build_record = request["state_build_driver"]
    stdout_path = collection_root / "state-build.stdout.log"
    stderr_path = collection_root / "state-build.stderr.log"
    retained_root = collection_root / "state-build"
    retained_root.mkdir()
    with tempfile.TemporaryDirectory(
        prefix="pulp-a3-trace-producer-state-build-",
    ) as temporary:
        workspace = Path(temporary).resolve()
        source_archive, source_snapshot = role_support.export_exact_source_tree(
            source_root, request["source_revision"], workspace / "source",
            f"{request['state']} overhead source",
        )
        source_archive_digest = sha256_file(
            source_archive, "state source archive",
        )
        source_tree_digest = role_support.directory_digest(
            source_snapshot, "state source tree",
        )
        if source_archive_tree_sha256(
            source_archive, "state source archive",
        ) != source_tree_digest:
            raise OverheadError("state source archive and extracted tree differ")
        try:
            source_tree = role_support.git_output(
                source_root, "rev-parse", f"{request['source_revision']}^{{tree}}",
            )
        except role_support.ProducerError as error:
            raise OverheadError(str(error)) from error
        if GIT_REVISION.fullmatch(source_tree) is None:
            raise OverheadError("state source revision has an invalid tree identity")
        local_driver, observed_driver_digest = role_support.snapshot_file(
            build_driver, workspace / f"state-build-driver{build_driver.suffix}",
            "state build driver",
        )
        if observed_driver_digest != build_record["sha256"]:
            raise OverheadError("state build driver changed before execution")
        output = workspace / "output"
        build_request_path = workspace / "request.json"
        build_receipt_path = workspace / "receipt.json"
        build_request = {
            "schema": STATE_BUILD_REQUEST_SCHEMA,
            "version": 1,
            "attempt_nonce": attempt_nonce,
            "state": request["state"],
            "source_revision": request["source_revision"],
            "candidate_revision": request["candidate_revision"],
            "source_tree": source_tree,
            "source_tree_sha256": source_tree_digest,
            "source_archive_sha256": source_archive_digest,
            "source_directory": str(source_snapshot),
            "output_directory": str(output),
            "binary_sha256": request["binary_sha256"],
            "identity": state_build_identity(request),
            "tracing": request["tracing"],
            "driver_sha256": observed_driver_digest,
        }
        atomic_json(build_request_path, build_request)
        (workspace / "home").mkdir()
        (workspace / "tmp").mkdir()
        environment = {
            "HOME": str(workspace / "home"),
            "LC_ALL": "C",
            "PATH": ":".join((
                str(Path(sys.executable).resolve().parent), "/opt/homebrew/bin",
                "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin",
            )),
            "PYTHONFAULTHANDLER": "1",
            "PYTHONNOUSERSITE": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "TMPDIR": str(workspace / "tmp"),
        }
        for name in ("DEVELOPER_DIR", "SDKROOT"):
            if os.environ.get(name):
                environment[name] = os.environ[name]
        sandbox_profile = role_support.macos_default_deny_build_profile(
            workspace, [evidence_root, source_root, binary, driver, build_driver],
        )
        exit_code = role_support.bounded_run(
            ["/usr/bin/sandbox-exec", "-p", sandbox_profile, str(local_driver),
             "--request", str(build_request_path),
             "--receipt", str(build_receipt_path)],
            cwd=workspace, environment=environment, timeout_seconds=1800,
            stdout_path=stdout_path, stderr_path=stderr_path,
        )
        if not build_receipt_path.is_file() or build_receipt_path.is_symlink():
            stdout_tail = regular_file_bytes(
                stdout_path, "state build stdout",
            )[-2048:].decode(errors="replace").strip()
            stderr_tail = regular_file_bytes(
                stderr_path, "state build stderr",
            )[-4096:].decode(errors="replace").strip()
            detail = " | ".join(
                item for item in (stdout_tail, stderr_tail) if item
            )
            raise OverheadError(
                f"state build driver omitted its receipt (exit {exit_code})"
                + (f": {detail}" if detail else "")
            )
        receipt = regular_json(build_receipt_path, "state build receipt")
        exact_keys(receipt, STATE_BUILD_RECEIPT_KEYS, "state build receipt")
        if (
            exit_code != 0
            or receipt["schema"] != STATE_BUILD_RECEIPT_SCHEMA
            or receipt["version"] != 1
            or receipt["attempt_nonce"] != attempt_nonce
            or receipt["state"] != request["state"]
            or receipt["outcome"] != "pass"
            or receipt["reason"] is not None
            or receipt["source_revision"] != request["source_revision"]
            or receipt["candidate_revision"] != request["candidate_revision"]
            or receipt["source_tree"] != source_tree
            or receipt["source_tree_sha256"] != source_tree_digest
            or receipt["source_archive_sha256"] != source_archive_digest
            or receipt["binary_sha256"] != request["binary_sha256"]
            or receipt["identity"] != state_build_identity(request)
            or receipt["tracing"] != request["tracing"]
            or receipt["driver_sha256"] != observed_driver_digest
        ):
            raise OverheadError("state build receipt differs from the closed request")
        command = receipt["build_command"]
        if (
            not isinstance(command, list) or not 1 <= len(command) <= 64
            or any(not isinstance(item, str) or not item for item in command)
            or not isinstance(receipt["builder_id"], str)
            or not receipt["builder_id"]
        ):
            raise OverheadError("state build receipt lacks a bounded builder identity")
        try:
            started = role_support.parse_utc(
                receipt["build_started_utc"], "state build start",
            )
            finished = role_support.parse_utc(
                receipt["build_finished_utc"], "state build finish",
            )
        except role_support.ProducerError as error:
            raise OverheadError(str(error)) from error
        if finished < started:
            raise OverheadError("state build finishes before it starts")
        if not output.is_dir() or output.is_symlink():
            raise OverheadError("state build output is not a fresh regular directory")
        try:
            product = role_support.safe_built_path(
                output, receipt["product_path"], "state-built product",
            )
        except role_support.ProducerError as error:
            raise OverheadError(str(error)) from error
        product_digest = sha256_file(product, "state-built product")
        if (
            not product.is_file() or product.is_symlink()
            or not os.access(product, os.X_OK)
            or receipt["product_sha256"] != product_digest
            or product_digest != request["binary_sha256"]
            or product_digest != sha256_file(binary, "measured state product")
        ):
            raise OverheadError(
                "independent state build differs from the measured executable"
            )
        if [entry[0] for entry in role_support.directory_entries(
            output, "state build output",
        )] != [product.relative_to(output).as_posix()]:
            raise OverheadError("state build retained unrelated output")
        validate_binary_compile_config(
            product, bool(request["tracing"]["compiled_in"]),
            "state-built product",
        )
        toolchain = receipt["toolchain"]
        if not isinstance(toolchain, list) or not 1 <= len(toolchain) <= 16:
            raise OverheadError("state build lacks bounded toolchain provenance")
        retained_toolchain: list[dict[str, str]] = []
        observed_tool_paths: set[Path] = set()
        allowed_tool_roots = tuple(
            path.resolve() for path in (
                Path(sys.executable).resolve().parent.parent,
                Path("/System"), Path("/usr"), Path("/bin"), Path("/sbin"),
                Path("/Library/Developer"), Path("/Applications/Xcode.app"),
                Path("/opt/homebrew"), Path("/usr/local"),
            ) if path.exists()
        )
        for index, tool in enumerate(toolchain):
            exact_keys(tool, TOOLCHAIN_KEYS, f"state build toolchain[{index}]")
            tool_path = Path(tool["path"])
            resolved_tool = tool_path.resolve()
            if (
                not tool_path.is_absolute() or tool_path.is_symlink()
                or not resolved_tool.is_file() or not os.access(resolved_tool, os.X_OK)
                or resolved_tool in observed_tool_paths
                or not any(
                    resolved_tool == root or root in resolved_tool.parents
                    for root in allowed_tool_roots
                )
                or tool["path"] != str(resolved_tool)
                or tool["sha256"] != sha256_file(
                    resolved_tool, f"state build toolchain[{index}]",
                )
                or not isinstance(tool["version"], str) or not tool["version"]
            ):
                raise OverheadError(f"state build toolchain[{index}] is invalid")
            observed_tool_paths.add(resolved_tool)
            retained_path = (
                evidence_root / "tooling"
                / f"state-build-tool-{tool['sha256']}"
            )
            if retained_path.exists():
                if sha256_file(retained_path, "retained state build tool") != tool["sha256"]:
                    raise OverheadError("retained state build tool digest differs")
            else:
                role_support.snapshot_file(
                    resolved_tool, retained_path, f"state build toolchain[{index}]",
                )
            retained_toolchain.append(artifact_ref(retained_path, evidence_root))
        if command[0] != toolchain[0]["path"]:
            raise OverheadError("state build command is not toolchain bound")
        retained_source, _ = role_support.snapshot_file(
            source_archive, retained_root / "source.tar", "state source archive",
        )
        retained_driver, _ = role_support.snapshot_file(
            local_driver, retained_root / "build-driver", "state build driver",
        )
        retained_request, _ = role_support.snapshot_file(
            build_request_path, retained_root / "request.json", "state build request",
        )
        retained_receipt, _ = role_support.snapshot_file(
            build_receipt_path, retained_root / "receipt.json", "state build receipt",
        )
        retained_product, _ = role_support.snapshot_file(
            product, retained_root / "product", "state rebuilt product",
        )
    return {
        "source_archive": artifact_ref(retained_source, evidence_root),
        "build_driver": artifact_ref(retained_driver, evidence_root),
        "build_request": artifact_ref(retained_request, evidence_root),
        "build_receipt": artifact_ref(retained_receipt, evidence_root),
        "rebuilt_product": artifact_ref(retained_product, evidence_root),
        "stdout": artifact_ref(stdout_path, evidence_root),
        "stderr": artifact_ref(stderr_path, evidence_root),
        "toolchain": retained_toolchain,
    }


def collect_state(
    *, request_path: Path, evidence_root: Path, source_root: Path,
    binary: Path, driver: Path, build_driver: Path, output: Path,
    trace_processor: Path | None,
) -> dict[str, Any]:
    request = regular_json(request_path, "collection request")
    validate_collection_request(
        request, evidence_root=evidence_root, source_root=source_root,
        binary=binary, driver=driver, build_driver=build_driver,
    )
    try:
        request_path.resolve().relative_to(evidence_root.resolve())
        output.resolve().relative_to(evidence_root.resolve())
    except ValueError as error:
        raise OverheadError("collection request/output must remain under the evidence root") from error
    if output.exists() or output.is_symlink():
        raise OverheadError("collection raw output is not fresh")
    state = request["state"]
    collection_root = output.parent / f"collection-{state}"
    if collection_root.exists() or collection_root.is_symlink():
        raise OverheadError("collection artifact directory is not fresh")
    collection_root.mkdir(parents=True)
    driver_root = collection_root / "driver-artifacts"
    driver_root.mkdir()
    driver_snapshot, driver_digest = role_support.snapshot_file(
        driver, collection_root / "measurement-driver", "overhead measurement driver",
    )
    binary_digest = sha256_file(binary, "overhead product binary")
    attempt_nonce = os.urandom(16).hex()
    state_build = run_state_source_build(
        request=request, evidence_root=evidence_root, source_root=source_root,
        binary=binary, driver=driver, build_driver=build_driver,
        collection_root=collection_root, attempt_nonce=attempt_nonce,
    )
    monitor = CollectionLivenessMonitor(
        root=collection_root / "liveness", attempt_nonce=attempt_nonce,
        state=state, binary=binary, binary_sha256=binary_digest,
    )
    driver_request = {
        "schema": COLLECTION_DRIVER_REQUEST_SCHEMA,
        "version": 1,
        "attempt_nonce": attempt_nonce,
        "challenge_nonce": monitor.challenge_nonce,
        "state": state,
        "binary": {"runtime_path": str(binary.resolve()), "sha256": binary_digest},
        "counts": {"warmups": 5, "measured": 30, "fresh_process": 20},
        "tracing": request["tracing"],
        "trace_session_config": request["trace_session_config"],
        "artifact_directory": str(driver_root.resolve()),
    }
    driver_request.update({"liveness_challenge": monitor.contract()})
    driver_request_path = collection_root / "driver-request.json"
    driver_receipt_path = collection_root / "driver-receipt.json"
    atomic_json(driver_request_path, driver_request)
    monitor.start()
    try:
        exit_code = role_support.bounded_run(
            [str(driver_snapshot), "--request", str(driver_request_path),
             "--receipt", str(driver_receipt_path)],
            cwd=collection_root, environment=dict(os.environ), timeout_seconds=1800,
            stdout_path=collection_root / "driver.stdout.log",
            stderr_path=collection_root / "driver.stderr.log",
        )
    finally:
        observations, handshake_artifacts = monitor.finish()
    if len(observations) != 55:
        stderr_tail = regular_file_bytes(
            collection_root / "driver.stderr.log", "collection driver stderr",
        )[-4096:].decode(errors="replace").strip()
        raise OverheadError(
            "collection driver did not complete 55 live sample challenges"
            + (f": {stderr_tail}" if stderr_tail else "")
        )
    live_pids = sorted({
        item["host_pid"] for item in observations
        if role_support.process_exists(item["host_pid"])
    })
    if live_pids:
        raise OverheadError("collection driver left product process IDs alive")
    if exit_code != 0 or not driver_receipt_path.is_file() or driver_receipt_path.is_symlink():
        raise OverheadError("collection driver did not return a closed passing receipt")
    driver_receipt = regular_json(driver_receipt_path, "collection driver receipt")
    exact_keys(driver_receipt, COLLECTION_DRIVER_RECEIPT_KEYS, "collection driver receipt")
    if (
        driver_receipt["schema"] != COLLECTION_DRIVER_RECEIPT_SCHEMA
        or driver_receipt["version"] != 1
        or driver_receipt["attempt_nonce"] != attempt_nonce
        or driver_receipt["challenge_nonce"] != monitor.challenge_nonce
        or driver_receipt["state"] != state
        or driver_receipt["outcome"] != "pass"
        or driver_receipt["reason"] is not None
        or driver_receipt["binary_sha256"] != binary_digest
        or driver_receipt["driver_sha256"] != driver_digest
    ):
        raise OverheadError("collection driver receipt differs from the closed invocation")
    transcript_path = collection_root / "liveness-transcript.json"
    atomic_json(transcript_path, {
        "schema": COLLECTION_TRANSCRIPT_SCHEMA,
        "version": 1,
        "attempt_nonce": attempt_nonce,
        "challenge_nonce": monitor.challenge_nonce,
        "state": state,
        "binary_sha256": binary_digest,
        "driver_sha256": driver_digest,
        "observations": observations,
    })
    samples = driver_receipt["samples"]
    if not isinstance(samples, dict):
        raise OverheadError("collection driver receipt lacks sample sections")
    raw_samples: dict[str, list[dict[str, Any]]] = {}
    for section, count in (("warmups", 5), ("measured", 30), ("fresh_process", 20)):
        rows = samples.get(section)
        if not isinstance(rows, list) or len(rows) != count:
            raise OverheadError(f"collection driver lacks {count} {section} samples")
        raw_samples[section] = []
        for sequence, row in enumerate(rows):
            exact_keys(row, SAMPLE_KEYS, f"collection {section}[{sequence}]")
            raw_samples[section].append({
                "sequence": row["sequence"],
                "evidence_id": row["evidence_id"],
                "duration_ms": row["duration_ms"],
                "runtime_metrics": projected_driver_ref(
                    row["runtime_metrics"], driver_root=driver_root,
                    evidence_root=evidence_root,
                    label=f"collection {section}[{sequence}].runtime_metrics",
                ),
                "trace": (
                    projected_driver_ref(
                        row["trace"], driver_root=driver_root,
                        evidence_root=evidence_root,
                        label=f"collection {section}[{sequence}].trace",
                    ) if row["trace"] is not None else None
                ),
            })
    collection_receipt_path = collection_root / "collection-receipt.json"
    collection_receipt = {
        "schema": COLLECTION_RECEIPT_SCHEMA,
        "version": 1,
        "attempt_nonce": attempt_nonce,
        "challenge_nonce": monitor.challenge_nonce,
        "state": state,
        "source_revision": request["source_revision"],
        "binary_sha256": binary_digest,
        "driver_sha256": driver_digest,
        "collector_sha256": sha256_file(Path(__file__).resolve(), "A3 overhead collector"),
        "driver_artifact_directory": driver_root.resolve().relative_to(
            evidence_root.resolve()
        ).as_posix(),
        "driver_request": artifact_ref(driver_request_path, evidence_root),
        "driver_receipt": artifact_ref(driver_receipt_path, evidence_root),
        "liveness_transcript": artifact_ref(transcript_path, evidence_root),
        "handshake_artifacts": [
            artifact_ref(path, evidence_root) for path in handshake_artifacts
        ],
        "state_build": state_build,
    }
    atomic_json(collection_receipt_path, collection_receipt)
    raw = {
        **request,
        "schema": RAW_SCHEMA,
        "collection": artifact_ref(collection_receipt_path, evidence_root),
        **raw_samples,
    }
    validate_raw(
        raw, state=state, evidence_root=evidence_root,
        trace_processor=trace_processor, label=state,
        allow_fixture_collection=False, allow_fixture_chrome_json=False,
    )
    atomic_json(output, raw)
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    collect = subparsers.add_parser("collect-state")
    collect.add_argument("--request", required=True, type=Path)
    collect.add_argument("--evidence-root", required=True, type=Path)
    collect.add_argument("--source-root", required=True, type=Path)
    collect.add_argument("--binary", required=True, type=Path)
    collect.add_argument("--driver", required=True, type=Path)
    collect.add_argument("--build-driver", required=True, type=Path)
    collect.add_argument("--trace-processor", type=Path)
    collect.add_argument("--output", required=True, type=Path)
    ratify = subparsers.add_parser("ratify")
    for state in STATES:
        ratify.add_argument(f"--{state}", required=True, type=Path)
    ratify.add_argument("--evidence-root", required=True, type=Path)
    ratify.add_argument("--trace-processor", type=Path)
    ratify.add_argument("--generated-utc", required=True)
    ratify.add_argument("--output", required=True, type=Path)
    verify = subparsers.add_parser("verify")
    verify.add_argument("receipt", type=Path)
    verify.add_argument("--evidence-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        evidence_root = args.evidence_root.resolve()
        if args.command == "collect-state":
            raw = collect_state(
                request_path=args.request.resolve(), evidence_root=evidence_root,
                source_root=args.source_root.resolve(), binary=args.binary.resolve(),
                driver=args.driver.resolve(), build_driver=args.build_driver.resolve(),
                output=args.output.resolve(),
                trace_processor=(
                    args.trace_processor.resolve() if args.trace_processor else None
                ),
            )
            print(f"A3 trace producer collection: {raw['state']}: pass")
            return 0
        if args.command == "ratify":
            paths = {
                state: getattr(args, state.replace("-", "_")).resolve()
                for state in STATES
            }
            receipt = build_receipt(
                paths, evidence_root=evidence_root,
                generated_utc=args.generated_utc,
                trace_processor_path=(
                    args.trace_processor.resolve() if args.trace_processor else None
                ),
            )
            atomic_json(args.output.resolve(), receipt)
            print(f"A3 trace producer overhead: {receipt['verdict']}")
            return 0 if receipt["verdict"] == "pass" else 1
        validate_receipt(
            regular_json(args.receipt, "producer-overhead receipt"), evidence_root,
        )
        print("A3 trace producer overhead: pass")
        return 0
    except (OverheadError, ValueError) as error:
        print(f"A3 trace producer overhead: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
