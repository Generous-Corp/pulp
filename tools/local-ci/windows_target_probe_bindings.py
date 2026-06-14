"""Compatibility installer for Windows target probe/detail helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from windows_target_desktop_detail_bindings import (
    WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS,
    install_windows_target_desktop_detail_helpers,
    windows_desktop_session_state,
    windows_desktop_session_user,
    windows_repo_checkout_detail,
)
from windows_target_tooling_probe_bindings import (
    WINDOWS_TARGET_TOOLING_PROBE_EXPORTS,
    install_windows_target_tooling_probe_helpers,
    windows_remote_tooling_ready,
    windows_tooling_detail,
)


WINDOWS_TARGET_PROBE_EXPORTS = (
    *WINDOWS_TARGET_TOOLING_PROBE_EXPORTS,
    *WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS,
)


def install_windows_target_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_PROBE_EXPORTS,
) -> None:
    tooling_names = tuple(name for name in names if name in WINDOWS_TARGET_TOOLING_PROBE_EXPORTS)
    desktop_detail_names = tuple(name for name in names if name in WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS)
    known_names = set(WINDOWS_TARGET_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_target_tooling_probe_helpers(bindings, tooling_names)
    install_windows_target_desktop_detail_helpers(bindings, desktop_detail_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
