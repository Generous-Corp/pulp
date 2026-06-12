"""Facade dependency bindings for Windows target probe/detail helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


def windows_tooling_detail(
    bindings: dict,
    probe: dict,
    tool_name: str,
    *,
    missing_hint: str | None = None,
) -> str:
    return _binding(bindings, "_windows_target").windows_tooling_detail(
        probe,
        tool_name,
        missing_hint=missing_hint,
    )


def windows_remote_tooling_ready(bindings: dict, probe: dict) -> bool:
    return _binding(bindings, "_windows_target").windows_remote_tooling_ready(
        probe,
        required_tools=_binding(bindings, "WINDOWS_REQUIRED_REMOTE_TOOLS"),
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
