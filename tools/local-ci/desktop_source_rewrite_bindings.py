"""Compatibility facade for desktop source command rewrite bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_source_rewrite_command_bindings import (
    DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
    command_path_rewrite_candidate,
    install_desktop_source_rewrite_command_helpers,
    rewrite_launch_command_for_mapper,
)
from desktop_source_rewrite_root_bindings import (
    DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS,
    install_desktop_source_rewrite_root_helpers,
    rewrite_launch_command_for_posix_root,
    rewrite_launch_command_for_source_root,
    rewrite_launch_command_for_windows_root,
)


DESKTOP_SOURCE_REWRITE_EXPORTS = (
    *DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
    *DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS,
)


def install_desktop_source_rewrite_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REWRITE_EXPORTS,
) -> None:
    command_names = tuple(name for name in names if name in DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS)
    root_names = tuple(name for name in names if name in DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS)
    known_names = set(DESKTOP_SOURCE_REWRITE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_source_rewrite_command_helpers(bindings, command_names)
    install_desktop_source_rewrite_root_helpers(bindings, root_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
