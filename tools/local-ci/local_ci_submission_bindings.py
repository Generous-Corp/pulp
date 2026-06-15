"""Bindings from the local_ci facade to shared local-CI submission helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from local_ci_submission_dependency_bindings import local_ci_submission_dependencies


LOCAL_CI_SUBMISSION_EXPORTS = (
    "resolve_submission_options",
)


def resolve_submission_options(bindings: Mapping[str, Any], args: Any, command: str) -> tuple[dict, str, str, list[str], str, str, dict]:
    return _binding(bindings, "_local_ci_commands_cli").resolve_submission_options(
        args,
        command,
        **local_ci_submission_dependencies(bindings),
    )
