"""Bindings from the local_ci facade to Windows validation script construction."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS = ("windows_validation_script",)


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


def install_execution_runner_windows_script_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS,
) -> None:
    known_names = set(EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS)
    script_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), script_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
