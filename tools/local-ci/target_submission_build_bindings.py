"""Facade bindings for target submission metadata construction."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


TARGET_SUBMISSION_BUILD_EXPORTS = (
    "build_submission_metadata",
)


def build_submission_metadata(
    bindings: Mapping[str, Any],
    config: dict,
    branch: str,
    sha: str,
    targets: list[str],
    priority: str,
    validation: str,
    *,
    allow_root_mismatch: bool,
    allow_unreachable_targets: bool,
) -> dict:
    return _binding(bindings, "_target_preflight").build_submission_metadata(
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=allow_root_mismatch,
        allow_unreachable_targets=allow_unreachable_targets,
        root=_binding(bindings, "ROOT"),
        cwd_fn=Path.cwd,
        git_root_for_fn=_binding(bindings, "git_root_for"),
        config_path_fn=_binding(bindings, "config_path"),
        config_source_name_fn=_binding(bindings, "config_source_name"),
        preflight_target_host_state_fn=_binding(bindings, "preflight_target_host_state"),
        find_material_config_drift_fn=_binding(bindings, "find_material_config_drift"),
        normalize_provenance_fn=_binding(bindings, "normalize_provenance"),
        environ=_binding(bindings, "os").environ,
    )


def install_target_submission_build_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_SUBMISSION_BUILD_EXPORTS,
) -> None:
    known_names = set(TARGET_SUBMISSION_BUILD_EXPORTS)
    build_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), build_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
