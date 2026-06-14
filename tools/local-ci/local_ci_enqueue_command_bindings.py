"""Facade bindings for the local-CI enqueue command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


LOCAL_CI_ENQUEUE_COMMAND_EXPORTS = (
    "cmd_enqueue",
)


def cmd_enqueue(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_enqueue(
        args,
        resolve_submission_options_fn=_binding(bindings, "resolve_submission_options"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        enqueue_command_result_line_fn=_binding(bindings, "enqueue_command_result_line"),
    )
