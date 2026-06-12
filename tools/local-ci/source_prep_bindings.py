"""Compatibility facade for desktop source-prep dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_exact_source_bindings import (
    DESKTOP_EXACT_SOURCE_EXPORTS,
    local_worktree_matches,
    prepare_linux_exact_sha_source,
    prepare_macos_exact_sha_source,
    prepare_windows_exact_sha_source,
    reset_local_worktree,
)
from desktop_source_request_bindings import (
    DESKTOP_SOURCE_REQUEST_EXPORTS,
    attach_desktop_source_to_manifest,
    desktop_source_cache_key,
    desktop_source_root,
    make_desktop_source_request,
    split_windows_prepare_commands,
    validate_windows_prepare_commands,
)
from desktop_source_rewrite_bindings import (
    DESKTOP_SOURCE_REWRITE_EXPORTS,
    command_path_rewrite_candidate,
    rewrite_launch_command_for_mapper,
    rewrite_launch_command_for_posix_root,
    rewrite_launch_command_for_source_root,
    rewrite_launch_command_for_windows_root,
)


SOURCE_PREP_EXPORTS = (
    *DESKTOP_SOURCE_REQUEST_EXPORTS,
    *DESKTOP_SOURCE_REWRITE_EXPORTS,
    *DESKTOP_EXACT_SOURCE_EXPORTS,
)


def install_source_prep_helpers(bindings: dict[str, Any], names: tuple[str, ...] = SOURCE_PREP_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
