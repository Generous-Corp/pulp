"""Bindings from the local_ci facade to target submission metadata helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from binding_utils import print_binding as _print_binding


TARGET_SUBMISSION_EXPORTS = (
    "build_submission_metadata",
    "print_submission_metadata",
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


def print_submission_metadata(bindings: Mapping[str, Any], metadata: dict) -> None:
    return _binding(bindings, "_target_preflight").print_submission_metadata(
        metadata,
        short_sha_fn=_binding(bindings, "short_sha"),
        provenance_summary_fn=_binding(bindings, "provenance_summary"),
        print_fn=_print_binding(bindings),
    )


def install_target_submission_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_SUBMISSION_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
