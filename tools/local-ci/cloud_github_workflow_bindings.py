"""Bindings from the local_ci facade to cloud GitHub workflow helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_GITHUB_WORKFLOW_EXPORTS = (
    "gh_available",
    "gh_workflow_dispatch",
    "gh_run_view",
)


def gh_available(bindings: Mapping[str, Any]) -> bool:
    return _binding(bindings, "_cloud").gh_available()


def gh_workflow_dispatch(
    bindings: Mapping[str, Any],
    repository: str,
    workflow_file: str,
    ref: str,
    fields: dict[str, str],
) -> None:
    return _binding(bindings, "_cloud").gh_workflow_dispatch(repository, workflow_file, ref, fields)


def gh_run_view(bindings: Mapping[str, Any], repository: str, run_id: int) -> dict | None:
    return _binding(bindings, "_cloud").gh_run_view(repository, run_id)


def install_cloud_github_workflow_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_GITHUB_WORKFLOW_EXPORTS,
) -> None:
    known_names = set(CLOUD_GITHUB_WORKFLOW_EXPORTS)
    workflow_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), workflow_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
