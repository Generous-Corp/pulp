"""Bindings from the local_ci facade to desktop source request helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


DESKTOP_SOURCE_REQUEST_EXPORTS = (
    "make_desktop_source_request",
    "desktop_source_cache_key",
    "desktop_source_root",
    "split_windows_prepare_commands",
    "validate_windows_prepare_commands",
    "attach_desktop_source_to_manifest",
)


def make_desktop_source_request(bindings: Mapping[str, Any], args: Any) -> dict:
    return _binding(bindings, "_source_prep").make_desktop_source_request(
        args,
        normalize_desktop_source_mode_fn=_binding(bindings, "normalize_desktop_source_mode"),
        current_branch_fn=_binding(bindings, "current_branch"),
        current_sha_fn=_binding(bindings, "current_sha"),
    )


def desktop_source_cache_key(bindings: Mapping[str, Any], source_request: dict) -> str:
    return _binding(bindings, "_source_prep").desktop_source_cache_key(source_request)


def desktop_source_root(bindings: Mapping[str, Any], target_name: str, source_request: dict) -> Path:
    return _binding(bindings, "_source_prep").desktop_source_root(
        target_name,
        source_request,
        state_dir_fn=_binding(bindings, "state_dir"),
    )


def split_windows_prepare_commands(bindings: Mapping[str, Any], command: str) -> list[str]:
    return _binding(bindings, "_source_prep").split_windows_prepare_commands(command)


def validate_windows_prepare_commands(bindings: Mapping[str, Any], commands: list[str]) -> None:
    return _binding(bindings, "_source_prep").validate_windows_prepare_commands(commands)


def attach_desktop_source_to_manifest(
    bindings: Mapping[str, Any],
    manifest: dict,
    source_context: dict | None,
) -> None:
    return _binding(bindings, "_source_prep").attach_desktop_source_to_manifest(manifest, source_context)
