"""Dependency assembly for logs command execution bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def logs_run_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "resolve_job_for_logs_fn": _binding(bindings, "resolve_job_for_logs"),
        "target_log_path_fn": _binding(bindings, "target_log_path"),
        "job_logs_dir_fn": _binding(bindings, "job_logs_dir"),
        "tail_lines_fn": _binding(bindings, "tail_lines"),
        "missing_job_logs_line_fn": _binding(bindings, "missing_job_logs_line"),
        "missing_log_files_line_fn": _binding(bindings, "missing_log_files_line"),
        "job_logs_header_line_fn": _binding(bindings, "job_logs_header_line"),
        "log_section_header_line_fn": _binding(bindings, "log_section_header_line"),
        "empty_log_line_fn": _binding(bindings, "empty_log_line"),
    }
