"""Compatibility facade for exact-SHA desktop source materialization helpers."""

from __future__ import annotations

from source_prep_exact_linux import prepare_linux_exact_sha_source
from source_prep_exact_local import local_worktree_matches, reset_local_worktree
from source_prep_exact_macos import prepare_macos_exact_sha_source
from source_prep_exact_windows import prepare_windows_exact_sha_source


__all__ = (
    "local_worktree_matches",
    "prepare_linux_exact_sha_source",
    "prepare_macos_exact_sha_source",
    "prepare_windows_exact_sha_source",
    "reset_local_worktree",
)
