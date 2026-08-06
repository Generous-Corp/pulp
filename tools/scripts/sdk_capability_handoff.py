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
MAX_JSON_BYTES = 16 * 1024 * 1024


class HandoffError(RuntimeError):
    pass


def importer_path(platform: str) -> Path:
    if platform.startswith("windows-"):
        return Path("bin/pulp-import-design.exe")
    if platform.startswith(("darwin-", "linux-")):
        return Path("bin/pulp-import-design")
    raise HandoffError(f"unsupported SDK platform: {platform!r}")


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
    prefix: Path, *, sdk_source_sha: str, platform: str
) -> dict[str, object]:
    capability_bytes = _read_bytes(prefix / CAPABILITIES_PATH)
    capability_document = _json(capability_bytes, str(CAPABILITIES_PATH))
    capability_schema = _json(
        _read_bytes(prefix / CAPABILITIES_SCHEMA_PATH),
        str(CAPABILITIES_SCHEMA_PATH),
    )
    _validate(capability_document, capability_schema, str(CAPABILITIES_PATH))
    importer = importer_path(platform)
    importer_bytes = _read_bytes(prefix / importer, limit=512 * 1024 * 1024)
    document: dict[str, object] = {
        "$schema": HANDOFF_SCHEMA_PATH.name,
        "schema": SCHEMA,
        "sdk_source_sha": sdk_source_sha,
        "platform": platform,
        "importer": {
            "path": importer.as_posix(),
            "sha256": sha256(importer_bytes),
        },
        "agent_capabilities": {
            "path": CAPABILITIES_PATH.as_posix(),
            "sha256": sha256(capability_bytes),
            "content": capability_document,
        },
    }
    handoff_schema = _json(
        _read_bytes(prefix / HANDOFF_SCHEMA_PATH), str(HANDOFF_SCHEMA_PATH)
    )
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
    capability_bytes: bytes,
    capability_schema_bytes: bytes,
    expected_sdk_source_sha: str,
    expected_platform: str,
) -> dict[str, object]:
    document = _json(handoff_bytes, str(HANDOFF_PATH))
    handoff_schema = _json(handoff_schema_bytes, str(HANDOFF_SCHEMA_PATH))
    _validate(document, handoff_schema, str(HANDOFF_PATH))
    if not isinstance(document, dict):
        raise HandoffError(f"{HANDOFF_PATH} must contain an object")
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
    prefix: Path, *, expected_sdk_source_sha: str, expected_platform: str
) -> dict[str, object]:
    importer = importer_path(expected_platform)
    return verify_payload(
        handoff_bytes=_read_bytes(prefix / HANDOFF_PATH),
        handoff_schema_bytes=_read_bytes(prefix / HANDOFF_SCHEMA_PATH),
        importer_bytes=_read_bytes(prefix / importer, limit=512 * 1024 * 1024),
        capability_bytes=_read_bytes(prefix / CAPABILITIES_PATH),
        capability_schema_bytes=_read_bytes(prefix / CAPABILITIES_SCHEMA_PATH),
        expected_sdk_source_sha=expected_sdk_source_sha,
        expected_platform=expected_platform,
    )
