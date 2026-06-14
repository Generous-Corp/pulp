"""Facade bindings for logs job-resolution command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from logs_resolution_command_dependency_bindings import logs_resolution_command_dependencies


LOGS_RESOLUTION_COMMAND_EXPORTS = (
    "resolve_job_for_logs",
)


def resolve_job_for_logs(bindings: Mapping[str, Any], job_ref: str | None) -> dict | None:
    return _binding(bindings, "_logs_cli").resolve_job_for_logs(
        job_ref,
        **logs_resolution_command_dependencies(bindings),
    )


def install_logs_resolution_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOGS_RESOLUTION_COMMAND_EXPORTS,
) -> None:
    known_names = set(LOGS_RESOLUTION_COMMAND_EXPORTS)
    resolution_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), resolution_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
