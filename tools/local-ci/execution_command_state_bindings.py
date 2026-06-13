"""Bindings from the local_ci facade to validation command state helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_COMMAND_STATE_EXPORTS = (
    "remote_commit_error",
    "prepared_state_root",
    "should_reuse_prepared_state",
)


def remote_commit_error(bindings: Mapping[str, Any], target_name: str, host: str, job: dict) -> str:
    return _binding(bindings, "_execution").remote_commit_error(target_name, host, job)


def prepared_state_root(bindings: Mapping[str, Any], target_name: str, validation: str) -> Path:
    return _binding(bindings, "_execution").prepared_state_root(target_name, validation)


def should_reuse_prepared_state(bindings: Mapping[str, Any], job: dict) -> bool:
    return _binding(bindings, "_execution").should_reuse_prepared_state(job)


def install_execution_command_state_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_COMMAND_STATE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
