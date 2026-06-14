"""Facade bindings for cleanup command execution."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLEANUP_RUN_COMMAND_EXPORTS = (
    "cmd_cleanup",
)


def cmd_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cleanup_cli").cmd_cleanup(
        args,
        load_queue_fn=_binding(bindings, "load_queue"),
        collect_cleanup_plan_fn=_binding(bindings, "collect_local_ci_cleanup_plan"),
        apply_cleanup_plan_fn=_binding(bindings, "apply_local_ci_cleanup_plan"),
        print_cleanup_plan_fn=_binding(bindings, "print_local_ci_cleanup_plan"),
        print_state_footprint_fn=_binding(bindings, "print_local_ci_state_footprint"),
        format_size_fn=_binding(bindings, "format_size_bytes"),
        describe_path_fn=_binding(bindings, "describe_path_for_cleanup"),
    )


def install_cleanup_run_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_RUN_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLEANUP_RUN_COMMAND_EXPORTS)
    run_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
