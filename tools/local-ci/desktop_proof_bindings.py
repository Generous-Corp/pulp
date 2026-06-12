"""Bindings from the local_ci facade to desktop proof summary helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


DESKTOP_PROOF_EXPORTS = (
    "normalize_desktop_proof_source_mode",
    "desktop_manifest_adapter",
    "desktop_manifest_run_status",
    "desktop_manifest_source",
    "desktop_proof_scope_for_adapter",
    "desktop_run_summary",
    "desktop_proof_summaries",
)


def normalize_desktop_proof_source_mode(bindings: Mapping[str, Any], mode: str | None) -> str:
    return _binding(bindings, "_reporting").normalize_desktop_proof_source_mode(mode)


def desktop_manifest_adapter(bindings: Mapping[str, Any], config: dict, manifest: dict) -> str:
    return _binding(bindings, "_reporting").desktop_manifest_adapter(config, manifest)


def desktop_manifest_run_status(bindings: Mapping[str, Any], manifest: dict) -> str:
    return _binding(bindings, "_reporting").desktop_manifest_run_status(manifest)


def desktop_manifest_source(bindings: Mapping[str, Any], manifest: dict) -> dict:
    return _binding(bindings, "_reporting").desktop_manifest_source(manifest)


def desktop_proof_scope_for_adapter(bindings: Mapping[str, Any], adapter: str) -> str:
    return _binding(bindings, "_reporting").desktop_proof_scope_for_adapter(adapter)


def desktop_run_summary(bindings: Mapping[str, Any], config: dict, manifest: dict) -> dict:
    return _binding(bindings, "_reporting").desktop_run_summary(config, manifest)


def desktop_proof_summaries(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    source_mode: str | None = None,
    sha: str | None = None,
    branch: str | None = None,
    limit: int | None = None,
) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_proof_summaries(
        config,
        target_name=target_name,
        action=action,
        source_mode=source_mode,
        sha=sha,
        branch=branch,
        limit=limit,
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
    )
