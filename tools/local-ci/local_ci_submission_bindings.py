"""Bindings from the local_ci facade to shared local-CI submission helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


LOCAL_CI_SUBMISSION_EXPORTS = (
    "resolve_submission_options",
)


def resolve_submission_options(bindings: Mapping[str, Any], args: Any, command: str) -> tuple[dict, str, str, list[str], str, str, dict]:
    return _binding(bindings, "_local_ci_commands_cli").resolve_submission_options(
        args,
        command,
        load_config_fn=_binding(bindings, "load_config"),
        current_branch_fn=_binding(bindings, "current_branch"),
        resolve_git_ref_sha_fn=_binding(bindings, "resolve_git_ref_sha"),
        current_sha_fn=_binding(bindings, "current_sha"),
        resolve_targets_fn=_binding(bindings, "resolve_targets"),
        parse_targets_arg_fn=_binding(bindings, "parse_targets_arg"),
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        default_priority_for_fn=_binding(bindings, "default_priority_for"),
        normalize_validation_mode_fn=_binding(bindings, "normalize_validation_mode"),
        build_submission_metadata_fn=_binding(bindings, "build_submission_metadata"),
    )
