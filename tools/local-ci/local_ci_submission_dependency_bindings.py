"""Dependency assembly for shared local-CI submission option bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def local_ci_submission_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "current_branch_fn": _binding(bindings, "current_branch"),
        "resolve_git_ref_sha_fn": _binding(bindings, "resolve_git_ref_sha"),
        "current_sha_fn": _binding(bindings, "current_sha"),
        "resolve_targets_fn": _binding(bindings, "resolve_targets"),
        "parse_targets_arg_fn": _binding(bindings, "parse_targets_arg"),
        "normalize_priority_fn": _binding(bindings, "normalize_priority"),
        "default_priority_for_fn": _binding(bindings, "default_priority_for"),
        "normalize_validation_mode_fn": _binding(bindings, "normalize_validation_mode"),
        "build_submission_metadata_fn": _binding(bindings, "build_submission_metadata"),
    }
