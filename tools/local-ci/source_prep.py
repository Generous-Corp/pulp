"""Compatibility facade for desktop source-preparation helpers."""

from __future__ import annotations

from desktop_source_request_core import make_desktop_source_request
from desktop_source_request_manifest import attach_desktop_source_to_manifest
from desktop_source_request_path import desktop_source_cache_key, desktop_source_root
from desktop_source_request_windows import split_windows_prepare_commands, validate_windows_prepare_commands
from desktop_source_rewrite_command import command_path_rewrite_candidate, rewrite_launch_command_for_mapper
from desktop_source_rewrite_root import (
    rewrite_launch_command_for_posix_root,
    rewrite_launch_command_for_source_root,
    rewrite_launch_command_for_windows_root,
)
from source_prep_exact import (
    local_worktree_matches,
    prepare_linux_exact_sha_source,
    prepare_macos_exact_sha_source,
    prepare_windows_exact_sha_source,
    reset_local_worktree,
)


__all__ = (
    "attach_desktop_source_to_manifest",
    "command_path_rewrite_candidate",
    "desktop_source_cache_key",
    "desktop_source_root",
    "local_worktree_matches",
    "make_desktop_source_request",
    "prepare_linux_exact_sha_source",
    "prepare_macos_exact_sha_source",
    "prepare_windows_exact_sha_source",
    "reset_local_worktree",
    "rewrite_launch_command_for_mapper",
    "rewrite_launch_command_for_posix_root",
    "rewrite_launch_command_for_source_root",
    "rewrite_launch_command_for_windows_root",
    "split_windows_prepare_commands",
    "validate_windows_prepare_commands",
)
