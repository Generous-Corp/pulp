"""Bindings from the local_ci facade to the desktop publish report command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS = (
    "cmd_desktop_publish",
)


def cmd_desktop_publish(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_publish(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        stage_desktop_publish_report_fn=_binding(bindings, "stage_desktop_publish_report"),
        desktop_publish_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_publish_lines"),
    )


def install_desktop_report_publish_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
