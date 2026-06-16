"""Bindings from the local_ci facade to the desktop video info commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_VIDEO_INFO_COMMAND_EXPORTS = (
    "cmd_desktop_video_matrix",
)


def cmd_desktop_video_matrix(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_video_matrix_commands_cli").cmd_desktop_video_matrix(
        args,
        load_config_fn=_binding(bindings, "load_config"),
    )


def install_desktop_video_info_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_VIDEO_INFO_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_VIDEO_INFO_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
