"""Compatibility facade for desktop source command rewrite bindings."""

from __future__ import annotations

from desktop_source_rewrite_command_bindings import (
    DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
    command_path_rewrite_candidate,
    rewrite_launch_command_for_mapper,
)
from desktop_source_rewrite_root_bindings import (
    DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS,
    rewrite_launch_command_for_posix_root,
    rewrite_launch_command_for_source_root,
    rewrite_launch_command_for_windows_root,
)


DESKTOP_SOURCE_REWRITE_EXPORTS = (
    *DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
    *DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS,
)
