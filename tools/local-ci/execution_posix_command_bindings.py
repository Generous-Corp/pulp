"""Bindings from the local_ci facade to POSIX SSH validation command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_POSIX_COMMAND_EXPORTS = ("posix_ssh_validation_command",)


def posix_ssh_validation_command(
    bindings: Mapping[str, Any],
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str = "",
) -> tuple[list[str], str]:
    return _binding(bindings, "_execution").posix_ssh_validation_command(
        target_name,
        host,
        repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
    )


def install_execution_posix_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_POSIX_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
