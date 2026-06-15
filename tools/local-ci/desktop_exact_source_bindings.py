"""Compatibility facade for desktop exact-SHA source dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_exact_source_local_bindings import (
    DESKTOP_EXACT_SOURCE_LOCAL_EXPORTS,
    install_desktop_exact_source_local_helpers,
    local_worktree_matches,
    reset_local_worktree,
)
from desktop_exact_source_macos_bindings import (
    DESKTOP_EXACT_SOURCE_MACOS_EXPORTS,
    install_desktop_exact_source_macos_helpers,
    prepare_macos_exact_sha_source,
)
from desktop_exact_source_remote_bindings import (
    DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS,
    install_desktop_exact_source_remote_helpers,
    prepare_linux_exact_sha_source,
    prepare_windows_exact_sha_source,
)


DESKTOP_EXACT_SOURCE_EXPORTS = (
    *DESKTOP_EXACT_SOURCE_LOCAL_EXPORTS,
    *DESKTOP_EXACT_SOURCE_MACOS_EXPORTS,
    *DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS,
)


def install_desktop_exact_source_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_EXACT_SOURCE_EXPORTS,
) -> None:
    local_names = tuple(name for name in names if name in DESKTOP_EXACT_SOURCE_LOCAL_EXPORTS)
    macos_names = tuple(name for name in names if name in DESKTOP_EXACT_SOURCE_MACOS_EXPORTS)
    remote_names = tuple(name for name in names if name in DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS)
    known_names = set(DESKTOP_EXACT_SOURCE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_exact_source_local_helpers(bindings, local_names)
    install_desktop_exact_source_macos_helpers(bindings, macos_names)
    install_desktop_exact_source_remote_helpers(bindings, remote_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
