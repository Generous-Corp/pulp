#!/usr/bin/env python3
"""Stamp and verify the exact agent surface belonging to an official Pulp SDK."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from pathlib import Path
from typing import Any

from json_schema_lite import UnsupportedKeyword, validate


SCHEMA = "pulp.agent-capability-handoff.v1"
HANDOFF_PATH = Path("share/pulp/agent-capability-handoff.json")
HANDOFF_SCHEMA_PATH = Path("share/pulp/agent-capability-handoff.schema.json")
CAPABILITIES_PATH = Path("share/pulp/agent-capabilities.json")
CAPABILITIES_SCHEMA_PATH = Path("share/pulp/agent-capabilities.schema.json")
IMPORTER_RUNTIME_ROOT = Path("bin/browser_capture-v1")
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_IMPORTER_RUNTIME_BYTES = 256 * 1024 * 1024


class HandoffError(RuntimeError):
    pass


def importer_path(platform: str) -> Path:
    if platform.startswith("windows-"):
        return Path("bin/pulp-import-design.exe")
    if platform.startswith(("darwin-", "linux-")):
        return Path("bin/pulp-import-design")
    raise HandoffError(f"unsupported SDK platform: {platform!r}")


def _installed_importer_runtime(
    prefix: Path, expected_paths: set[str]
) -> dict[str, bytes]:
    root = prefix / IMPORTER_RUNTIME_ROOT
    try:
        children = sorted(root.iterdir())
    except OSError as exc:
        raise HandoffError(f"cannot read importer runtime {root}: {exc}") from exc
    if not children or any(not path.is_file() for path in children):
        raise HandoffError(f"invalid importer runtime directory: {root}")
    runtime = {
        path.relative_to(prefix).as_posix(): _read_bytes(
            path, limit=MAX_IMPORTER_RUNTIME_BYTES
        )
        for path in children
    }
    if set(runtime) != expected_paths:
        raise HandoffError(
            f"installed importer runtime does not match the selected contract"
        )
    return runtime


def _read_bytes(path: Path, *, limit: int = MAX_JSON_BYTES) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise HandoffError(f"cannot read {path}: {exc}") from exc
    if len(data) > limit:
        raise HandoffError(f"{path} exceeds {limit} bytes")
    return data


def _json(data: bytes, label: str) -> Any:
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HandoffError(f"{label} is not valid UTF-8 JSON: {exc}") from exc


def _validate(document: Any, schema: Any, label: str) -> None:
    try:
        problems = validate(document, schema)
    except UnsupportedKeyword as exc:
        raise HandoffError(f"{label} schema cannot be enforced: {exc}") from exc
    if problems:
        raise HandoffError(f"{label} violates its schema: {'; '.join(problems)}")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_handoff(
    prefix: Path,
    *,
    sdk_source_sha: str,
    platform: str,
    expected_importer_runtime_paths: set[str],
) -> dict[str, object]:
    capability_bytes = _read_bytes(prefix / CAPABILITIES_PATH)
    capability_document = _json(capability_bytes, str(CAPABILITIES_PATH))
    capability_schema_bytes = _read_bytes(prefix / CAPABILITIES_SCHEMA_PATH)
    capability_schema = _json(capability_schema_bytes, str(CAPABILITIES_SCHEMA_PATH))
    _validate(capability_document, capability_schema, str(CAPABILITIES_PATH))
    importer = importer_path(platform)
    importer_bytes = _read_bytes(prefix / importer, limit=512 * 1024 * 1024)
    runtime_bytes = _installed_importer_runtime(
        prefix, expected_importer_runtime_paths
    )
    runtime = [
        {"path": path, "sha256": sha256(payload)}
        for path, payload in runtime_bytes.items()
    ]
    document: dict[str, object] = {
        "$schema": HANDOFF_SCHEMA_PATH.name,
        "schema": SCHEMA,
        "sdk_source_sha": sdk_source_sha,
        "platform": platform,
        "schemas": {
            "handoff": {
                "path": HANDOFF_SCHEMA_PATH.as_posix(),
                "sha256": "",
            },
            "agent_capabilities": {
                "path": CAPABILITIES_SCHEMA_PATH.as_posix(),
                "sha256": sha256(capability_schema_bytes),
            },
        },
        "importer": {
            "path": importer.as_posix(),
            "sha256": sha256(importer_bytes),
            "runtime": runtime,
        },
        "agent_capabilities": {
            "path": CAPABILITIES_PATH.as_posix(),
            "sha256": sha256(capability_bytes),
            "content": capability_document,
        },
    }
    handoff_schema_bytes = _read_bytes(prefix / HANDOFF_SCHEMA_PATH)
    schemas = document["schemas"]
    assert isinstance(schemas, dict)
    handoff_descriptor = schemas["handoff"]
    assert isinstance(handoff_descriptor, dict)
    handoff_descriptor["sha256"] = sha256(handoff_schema_bytes)
    handoff_schema = _json(handoff_schema_bytes, str(HANDOFF_SCHEMA_PATH))
    _validate(document, handoff_schema, str(HANDOFF_PATH))
    return document


def write_atomically(path: Path, document: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(document, indent=2, sort_keys=True) + "\n"
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary_path = Path(temporary)
    try:
        fchmod = getattr(os, "fchmod", None)
        if fchmod is not None:
            fchmod(fd, 0o644)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def verify_payload(
    *,
    handoff_bytes: bytes,
    handoff_schema_bytes: bytes,
    importer_bytes: bytes,
    importer_runtime_bytes: dict[str, bytes],
    capability_bytes: bytes,
    capability_schema_bytes: bytes,
    expected_sdk_source_sha: str,
    expected_platform: str,
) -> dict[str, object]:
    document = _json(handoff_bytes, str(HANDOFF_PATH))
    if not isinstance(document, dict):
        raise HandoffError(f"{HANDOFF_PATH} must contain an object")
    schemas = document.get("schemas")
    if not isinstance(schemas, dict) or set(schemas) != {
        "handoff", "agent_capabilities"
    }:
        raise HandoffError(f"{HANDOFF_PATH}: schema descriptors are invalid")
    expected_schemas = {
        "handoff": (HANDOFF_SCHEMA_PATH, handoff_schema_bytes),
        "agent_capabilities": (
            CAPABILITIES_SCHEMA_PATH,
            capability_schema_bytes,
        ),
    }
    for name, (path, payload) in expected_schemas.items():
        descriptor = schemas.get(name)
        if not isinstance(descriptor, dict) or set(descriptor) != {"path", "sha256"}:
            raise HandoffError(
                f"{HANDOFF_PATH}: {name} schema descriptor is invalid"
            )
        if descriptor["path"] != path.as_posix():
            raise HandoffError(
                f"{HANDOFF_PATH}: {name} schema path is {descriptor['path']!r}, "
                f"expected {path.as_posix()!r}"
            )
        if descriptor["sha256"] != sha256(payload):
            raise HandoffError(
                f"{HANDOFF_PATH}: {name} schema sha256 does not match installed "
                f"{path}"
            )
    handoff_schema = _json(handoff_schema_bytes, str(HANDOFF_SCHEMA_PATH))
    _validate(document, handoff_schema, str(HANDOFF_PATH))
    if document.get("sdk_source_sha") != expected_sdk_source_sha:
        raise HandoffError(
            f"{HANDOFF_PATH}: sdk_source_sha is {document.get('sdk_source_sha')!r}, "
            f"expected {expected_sdk_source_sha!r}"
        )
    if document.get("platform") != expected_platform:
        raise HandoffError(
            f"{HANDOFF_PATH}: platform is {document.get('platform')!r}, "
            f"expected {expected_platform!r}"
        )
    expected_importer = importer_path(expected_platform).as_posix()
    importer = document["importer"]
    capabilities = document["agent_capabilities"]
    assert isinstance(importer, dict) and isinstance(capabilities, dict)
    if importer["path"] != expected_importer:
        raise HandoffError(
            f"{HANDOFF_PATH}: importer path is {importer['path']!r}, "
            f"expected {expected_importer!r}"
        )
    actual_importer_hash = sha256(importer_bytes)
    if importer["sha256"] != actual_importer_hash:
        raise HandoffError(
            f"{HANDOFF_PATH}: importer sha256 does not match installed {expected_importer}"
        )
    expected_runtime = set(importer_runtime_bytes)
    runtime_items = importer["runtime"]
    declared_runtime = {item["path"]: item["sha256"] for item in runtime_items}
    if len(declared_runtime) != len(runtime_items):
        raise HandoffError(f"{HANDOFF_PATH}: importer runtime paths are duplicated")
    if set(declared_runtime) != expected_runtime:
        raise HandoffError(
            f"{HANDOFF_PATH}: importer runtime paths do not match the selected contract"
        )
    for path in sorted(expected_runtime):
        if declared_runtime[path] != sha256(importer_runtime_bytes[path]):
            raise HandoffError(
                f"{HANDOFF_PATH}: importer runtime sha256 does not match installed {path}"
            )
    actual_capability_hash = sha256(capability_bytes)
    if capabilities["sha256"] != actual_capability_hash:
        raise HandoffError(
            f"{HANDOFF_PATH}: capability sha256 does not match installed "
            f"{CAPABILITIES_PATH}"
        )
    capability_document = _json(capability_bytes, str(CAPABILITIES_PATH))
    if capabilities["content"] != capability_document:
        raise HandoffError(
            f"{HANDOFF_PATH}: embedded capability content does not match installed "
            f"{CAPABILITIES_PATH}"
        )
    capability_schema = _json(capability_schema_bytes, str(CAPABILITIES_SCHEMA_PATH))
    _validate(capability_document, capability_schema, str(CAPABILITIES_PATH))
    return document


def verify_handoff(
    prefix: Path,
    *,
    expected_sdk_source_sha: str,
    expected_platform: str,
    expected_importer_runtime_paths: set[str],
) -> dict[str, object]:
    importer = importer_path(expected_platform)
    return verify_payload(
        handoff_bytes=_read_bytes(prefix / HANDOFF_PATH),
        handoff_schema_bytes=_read_bytes(prefix / HANDOFF_SCHEMA_PATH),
        importer_bytes=_read_bytes(prefix / importer, limit=512 * 1024 * 1024),
        importer_runtime_bytes=_installed_importer_runtime(
            prefix, expected_importer_runtime_paths
        ),
        capability_bytes=_read_bytes(prefix / CAPABILITIES_PATH),
        capability_schema_bytes=_read_bytes(prefix / CAPABILITIES_SCHEMA_PATH),
        expected_sdk_source_sha=expected_sdk_source_sha,
        expected_platform=expected_platform,
    )
