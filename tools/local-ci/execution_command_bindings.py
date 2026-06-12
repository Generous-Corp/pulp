"""Bindings from the local_ci facade to validation command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


EXECUTION_COMMAND_EXPORTS = (
    "remote_commit_error",
    "prepared_state_root",
    "should_reuse_prepared_state",
    "local_validation_command",
    "posix_ssh_validation_command",
    "windows_validation_script",
)


def remote_commit_error(bindings: Mapping[str, Any], target_name: str, host: str, job: dict) -> str:
    return _binding(bindings, "_execution").remote_commit_error(target_name, host, job)


def prepared_state_root(bindings: Mapping[str, Any], target_name: str, validation: str) -> Path:
    return _binding(bindings, "_execution").prepared_state_root(target_name, validation)


def should_reuse_prepared_state(bindings: Mapping[str, Any], job: dict) -> bool:
    return _binding(bindings, "_execution").should_reuse_prepared_state(job)


def local_validation_command(bindings: Mapping[str, Any], job: dict, exclude_tests: str = "") -> tuple[list[str], str]:
    return _binding(bindings, "_execution").local_validation_command(job, exclude_tests)


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


def windows_validation_script(
    bindings: Mapping[str, Any],
    target_name: str,
    host: str,
    effective_repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str,
    cmake_generator: str,
    resolved_platform: str,
    resolved_generator_instance: str,
) -> tuple[str, str]:
    return _binding(bindings, "_execution").windows_validation_script(
        target_name,
        host,
        effective_repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
        cmake_generator=cmake_generator,
        resolved_platform=resolved_platform,
        resolved_generator_instance=resolved_generator_instance,
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )
