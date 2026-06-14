"""Bindings from the local_ci facade to top-level CLI dispatch helpers."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


CLI_MAIN_DISPATCH_EXPORTS = ("dispatch_main_command",)


def dispatch_main_command(bindings: Mapping[str, Any], args: Any, print_help: Callable[[], None]) -> int:
    return _binding(bindings, "_cli_dispatch").dispatch_main_command(
        args,
        commands={
            "enqueue": _binding(bindings, "cmd_enqueue"),
            "drain": _binding(bindings, "cmd_drain"),
            "run": _binding(bindings, "cmd_run"),
            "ship": _binding(bindings, "cmd_ship"),
            "check": _binding(bindings, "cmd_check"),
            "list": _binding(bindings, "cmd_list"),
            "bump": _binding(bindings, "cmd_bump"),
            "cancel": _binding(bindings, "cmd_cancel"),
            "logs": _binding(bindings, "cmd_logs"),
            "cleanup": _binding(bindings, "cmd_cleanup"),
            "evidence": _binding(bindings, "cmd_evidence"),
            "status": _binding(bindings, "cmd_status"),
            "desktop": _binding(bindings, "cmd_desktop"),
        },
        cloud_commands={
            "workflows": _binding(bindings, "cmd_cloud_workflows"),
            "defaults": _binding(bindings, "cmd_cloud_defaults"),
            "history": _binding(bindings, "cmd_cloud_history"),
            "compare": _binding(bindings, "cmd_cloud_compare"),
            "recommend": _binding(bindings, "cmd_cloud_recommend"),
            "run": _binding(bindings, "cmd_cloud_run"),
            "status": _binding(bindings, "cmd_cloud_status"),
        },
        cloud_namespace_commands={
            "doctor": _binding(bindings, "cmd_cloud_namespace_doctor"),
            "setup": _binding(bindings, "cmd_cloud_namespace_setup"),
        },
        print_help=print_help,
    )


def install_cli_main_dispatch_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLI_MAIN_DISPATCH_EXPORTS,
) -> None:
    known_names = set(CLI_MAIN_DISPATCH_EXPORTS)
    dispatch_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), dispatch_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
