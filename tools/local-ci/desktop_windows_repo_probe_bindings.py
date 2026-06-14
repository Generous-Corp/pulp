"""Compatibility facade for desktop Windows repo probe bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_windows_repo_checkout_ensure_bindings import (
    DESKTOP_WINDOWS_REPO_CHECKOUT_ENSURE_EXPORTS,
    ensure_windows_remote_repo_checkout,
    install_desktop_windows_repo_checkout_ensure_helpers,
)
from desktop_windows_repo_checkout_probe_bindings import (
    DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS,
    install_desktop_windows_repo_checkout_probe_helpers,
    probe_windows_repo_checkout,
)


DESKTOP_WINDOWS_REPO_PROBE_EXPORTS = (
    *DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS,
    *DESKTOP_WINDOWS_REPO_CHECKOUT_ENSURE_EXPORTS,
)


def install_desktop_windows_repo_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_WINDOWS_REPO_PROBE_EXPORTS,
) -> None:
    probe_names = tuple(name for name in names if name in DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS)
    ensure_names = tuple(name for name in names if name in DESKTOP_WINDOWS_REPO_CHECKOUT_ENSURE_EXPORTS)
    known_names = set(DESKTOP_WINDOWS_REPO_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_windows_repo_checkout_probe_helpers(bindings, probe_names)
    install_desktop_windows_repo_checkout_ensure_helpers(bindings, ensure_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
