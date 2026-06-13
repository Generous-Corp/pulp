"""Bindings from the local_ci facade to Windows validation script helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


EXECUTION_WINDOWS_COMMAND_EXPORTS = ("windows_validation_script",)


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
