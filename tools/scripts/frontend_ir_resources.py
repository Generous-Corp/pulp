#!/usr/bin/env python3
"""ResourceIR assembly helpers for PulpFrontendIR reports."""

from __future__ import annotations

import hashlib
import mimetypes
import pathlib
from typing import Any

from frontend_ir_sources import WATCH_INPUT_KEYS, artifacts_from_input, resource_id_key
from frontend_ir_validation import ROUTE_LIVE_JS, ROUTE_NATIVE_CPP, is_non_negative_int


def route_usage_for_input(key: str, default_routes: list[str]) -> list[str]:
    normalized = resource_id_key(key)
    if normalized in {"bundle", "runtime_trace"}:
        return [ROUTE_LIVE_JS]
    if normalized in {"materialized_audit", "cpp", "native_audit"}:
        return [ROUTE_NATIVE_CPP]
    return default_routes


def local_resource_path(path: str, repo_root: pathlib.Path) -> pathlib.Path | None:
    candidate = pathlib.Path(path)
    if not candidate.is_absolute():
        candidate = repo_root / candidate
    try:
        if candidate.is_file():
            return candidate
    except OSError:
        return None
    return None


def path_byte_size(path: str, repo_root: pathlib.Path) -> int | None:
    candidate = local_resource_path(path, repo_root)
    if candidate is None:
        return None
    try:
        return candidate.stat().st_size
    except OSError:
        return None


def path_sha256(path: str, repo_root: pathlib.Path) -> str | None:
    candidate = local_resource_path(path, repo_root)
    if candidate is None:
        return None
    digest = hashlib.sha256()
    try:
        with candidate.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def mime_for_path(path: str) -> str | None:
    suffix = pathlib.Path(path).suffix.lower()
    custom = {
        ".jsx": "text/jsx",
        ".tsx": "text/tsx",
        ".mjs": "text/javascript",
        ".js": "text/javascript",
        ".json": "application/json",
        ".svg": "image/svg+xml",
    }
    if suffix in custom:
        return custom[suffix]
    mime, _ = mimetypes.guess_type(path)
    return mime


def input_resource(key: str, artifact: dict[str, str], default_routes: list[str], repo_root: pathlib.Path,
                   requested_by: str, index: int | None = None) -> dict[str, Any] | None:
    path_value = artifact.get("path", "")
    sha = artifact.get("sha256", "")

    if not path_value:
        return None

    resource_id = f"input.{resource_id_key(key)}"
    if index is not None:
        resource_id = f"{resource_id}.{index}"
    resource: dict[str, Any] = {
        "id": resource_id,
        "original_uri": path_value,
        "resolved_uri": path_value,
        "requested_by": [requested_by],
        "route_usage": route_usage_for_input(key, default_routes),
        "transforms": [],
        "watch": key in WATCH_INPUT_KEYS,
    }
    if sha:
        resource["sha256"] = sha
    else:
        computed_sha = path_sha256(path_value, repo_root)
        if computed_sha:
            resource["sha256"] = computed_sha
    byte_size = path_byte_size(path_value, repo_root)
    if byte_size is not None:
        resource["byte_size"] = byte_size
    mime = mime_for_path(path_value)
    if mime:
        resource["mime"] = mime
    return resource


def resources_from_manifest(route_manifest: dict[str, Any], default_routes: list[str],
                            repo_root: pathlib.Path) -> list[dict[str, Any]]:
    inputs = route_manifest.get("inputs", {})
    if not isinstance(inputs, dict):
        return []
    requested_by = str(route_manifest.get("fixture") or route_manifest.get("schema") or "route_manifest")
    resources = []
    for key, value in inputs.items():
        artifacts = artifacts_from_input(value)
        for index, artifact in enumerate(artifacts):
            resource = input_resource(
                key,
                artifact,
                default_routes,
                repo_root,
                requested_by,
                index if len(artifacts) > 1 else None,
            )
            if resource is not None:
                resources.append(resource)
    return resources


def resource_counts(resources: list[dict[str, Any]]) -> dict[str, int]:
    counts = {
        "total": len(resources),
        "with_sha256": 0,
        "with_byte_size": 0,
        "watchable": 0,
    }
    for resource in resources:
        if resource.get("sha256"):
            counts["with_sha256"] += 1
        if is_non_negative_int(resource.get("byte_size")):
            counts["with_byte_size"] += 1
        if resource.get("watch") is True:
            counts["watchable"] += 1
        for route in resource.get("route_usage", []) or []:
            if isinstance(route, str):
                counts[f"route_usage_{route}"] = counts.get(f"route_usage_{route}", 0) + 1
    return counts
