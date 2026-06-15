"""Facade bindings for cleanup artifact identity helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLEANUP_ARTIFACT_IDENTITY_EXPORTS = (
    "result_file_job_id",
    "artifact_entry_sort_key",
)


def result_file_job_id(bindings: Mapping[str, Any], path: Any) -> str | None:
    return _binding(bindings, "_cleanup").result_file_job_id(path)


def artifact_entry_sort_key(bindings: Mapping[str, Any], entry: dict) -> tuple[float, str]:
    return _binding(bindings, "_cleanup").artifact_entry_sort_key(entry)


def install_cleanup_artifact_identity_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_ARTIFACT_IDENTITY_EXPORTS,
) -> None:
    known_names = set(CLEANUP_ARTIFACT_IDENTITY_EXPORTS)
    identity_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), identity_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
