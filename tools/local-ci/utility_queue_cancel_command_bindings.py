"""Facade bindings for queue cancel utility command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS = (
    "cmd_cancel",
)


def cmd_cancel(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_queue_commands_cli").cmd_cancel(
        args,
        cancel_queue_command_job_fn=_binding(bindings, "cancel_queue_command_job"),
        cancel_queue_command_result_line_fn=_binding(bindings, "cancel_queue_command_result_line"),
    )


def install_utility_queue_cancel_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS,
) -> None:
    known_names = set(UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS)
    cancel_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), cancel_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
