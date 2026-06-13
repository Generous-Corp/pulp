"""Facade bindings for local validation runner helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


EXECUTION_RUNNER_LOCAL_EXPORTS = ("run_local_validation",)


def run_local_validation(
    bindings: Mapping[str, Any],
    job: dict,
    exclude_tests: str = "",
    report_progress=None,
) -> dict:
    return _binding(bindings, "_execution").run_local_validation(
        job,
        exclude_tests,
        report_progress,
        root=_binding(bindings, "ROOT"),
        print_fn=_print_binding(bindings),
        short_sha_fn=_binding(bindings, "short_sha"),
        prepare_target_log_fn=_binding(bindings, "prepare_target_log"),
        now_iso_fn=_binding(bindings, "now_iso"),
        local_validation_command_fn=_binding(bindings, "local_validation_command"),
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        validation_result_from_run_fn=_binding(bindings, "validation_result_from_run"),
    )


def install_execution_runner_local_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RUNNER_LOCAL_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
