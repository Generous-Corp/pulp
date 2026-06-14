"""Bindings from the local_ci facade to logs command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


LOGS_COMMAND_EXPORTS = (
    "resolve_job_for_logs",
    "cmd_logs",
)


def resolve_job_for_logs(bindings: Mapping[str, Any], job_ref: str | None) -> dict | None:
    return _binding(bindings, "_logs_cli").resolve_job_for_logs(
        job_ref,
        load_queue_fn=_binding(bindings, "load_queue"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        select_job_for_logs_fn=_binding(bindings, "_queue_orchestrator").select_job_for_logs,
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
