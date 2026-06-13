"""Compatibility facade for desktop exact-SHA source dependency bindings."""

from __future__ import annotations

from desktop_exact_source_local_bindings import (
    local_worktree_matches,
    reset_local_worktree,
)
from desktop_exact_source_macos_bindings import prepare_macos_exact_sha_source
from desktop_exact_source_remote_bindings import (
    prepare_linux_exact_sha_source,
    prepare_windows_exact_sha_source,
)


DESKTOP_EXACT_SOURCE_EXPORTS = (
    "local_worktree_matches",
    "reset_local_worktree",
    "prepare_macos_exact_sha_source",
    "prepare_linux_exact_sha_source",
    "prepare_windows_exact_sha_source",
)

