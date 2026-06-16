"""Bindings for Windows target desktop-session and checkout detail helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS = (
    "windows_desktop_session_user",
    "windows_desktop_session_state",
    "windows_repo_checkout_detail",
)


def windows_desktop_session_user(bindings: dict, probe: dict | None) -> str:
    return _binding(bindings, "_windows_target").windows_desktop_session_user(probe)


def windows_desktop_session_state(bindings: dict, probe: dict | None) -> str:
    return _binding(bindings, "_windows_target").windows_desktop_session_state(probe)


def windows_repo_checkout_detail(
    bindings: dict,
    probe: dict | None,
    *,
    fallback_path: str | None = None,
) -> str:
    return _binding(bindings, "_windows_target").windows_repo_checkout_detail(
        probe,
        fallback_path=fallback_path,
    )


def install_windows_target_desktop_detail_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
