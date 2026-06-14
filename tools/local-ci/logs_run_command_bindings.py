"""Facade bindings for logs command execution."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


LOGS_RUN_COMMAND_EXPORTS = (
    "cmd_logs",
)


def cmd_logs(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_logs_cli").cmd_logs(
        args,
        resolve_job_for_logs_fn=_binding(bindings, "resolve_job_for_logs"),
        target_log_path_fn=_binding(bindings, "target_log_path"),
        job_logs_dir_fn=_binding(bindings, "job_logs_dir"),
        tail_lines_fn=_binding(bindings, "tail_lines"),
        missing_job_logs_line_fn=_binding(bindings, "missing_job_logs_line"),
        missing_log_files_line_fn=_binding(bindings, "missing_log_files_line"),
        job_logs_header_line_fn=_binding(bindings, "job_logs_header_line"),
        log_section_header_line_fn=_binding(bindings, "log_section_header_line"),
        empty_log_line_fn=_binding(bindings, "empty_log_line"),
    )


def install_logs_run_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOGS_RUN_COMMAND_EXPORTS,
) -> None:
    known_names = set(LOGS_RUN_COMMAND_EXPORTS)
    run_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
