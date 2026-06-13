"""Bindings from the local_ci facade to validation result I/O helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


EXECUTION_RESULT_IO_EXPORTS = (
    "save_result",
    "print_result",
)


def save_result(bindings: Mapping[str, Any], result: dict) -> Any:
    return _binding(bindings, "_execution").save_result(
        result,
        ensure_state_dirs_fn=_binding(bindings, "ensure_state_dirs"),
        results_dir_fn=_binding(bindings, "results_dir"),
        update_evidence_index_fn=_binding(bindings, "update_evidence_index"),
        now_fn=_binding(bindings, "datetime").now,
    )


def print_result(bindings: Mapping[str, Any], result: dict, result_path=None) -> None:
    return _binding(bindings, "_execution").print_result(
        result,
        result_path,
        normalize_result_fn=_binding(bindings, "normalize_result"),
        result_validation_line_fn=_binding(bindings, "result_validation_line"),
        result_execution_line_fn=_binding(bindings, "result_execution_line"),
        result_target_lines_fn=_binding(bindings, "result_target_lines"),
        result_overall_line_fn=_binding(bindings, "result_overall_line"),
        print_fn=_print_binding(bindings),
    )


def install_execution_result_io_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RESULT_IO_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
