"""Bindings from the local_ci facade to desktop proof summary helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_PROOF_SUMMARY_EXPORTS = (
    "normalize_desktop_proof_source_mode",
    "desktop_manifest_adapter",
    "desktop_manifest_run_status",
    "desktop_manifest_source",
    "desktop_proof_scope_for_adapter",
    "desktop_run_summary",
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


def install_desktop_proof_summary_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_PROOF_SUMMARY_EXPORTS,
) -> None:
    known_names = set(DESKTOP_PROOF_SUMMARY_EXPORTS)
    summary_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), summary_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
