"""Compatibility facade for remote exact-source preparation bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_exact_source_linux_bindings import (
    DESKTOP_EXACT_SOURCE_LINUX_EXPORTS,
    install_desktop_exact_source_linux_helpers,
    prepare_linux_exact_sha_source,
)
from desktop_exact_source_windows_bindings import (
    DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS,
    install_desktop_exact_source_windows_helpers,
    prepare_windows_exact_sha_source,
)


DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS = (
    *DESKTOP_EXACT_SOURCE_LINUX_EXPORTS,
    *DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS,
)


def install_desktop_exact_source_remote_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS,
) -> None:
    linux_names = tuple(name for name in names if name in DESKTOP_EXACT_SOURCE_LINUX_EXPORTS)
    windows_names = tuple(name for name in names if name in DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS)
    known_names = set(DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_exact_source_linux_helpers(bindings, linux_names)
    install_desktop_exact_source_windows_helpers(bindings, windows_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
