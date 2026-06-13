"""Bindings from the local_ci facade to queue utility command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


UTILITY_QUEUE_COMMAND_EXPORTS = (
    "cmd_bump",
    "cmd_cancel",
)


def cmd_bump(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_queue_commands_cli").cmd_bump(
        args,
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        bump_queue_command_job_fn=_binding(bindings, "bump_queue_command_job"),
        bump_queue_command_result_line_fn=_binding(bindings, "bump_queue_command_result_line"),
    )


def cmd_cancel(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_queue_commands_cli").cmd_cancel(
        args,
        cancel_queue_command_job_fn=_binding(bindings, "cancel_queue_command_job"),
        cancel_queue_command_result_line_fn=_binding(bindings, "cancel_queue_command_result_line"),
    )
