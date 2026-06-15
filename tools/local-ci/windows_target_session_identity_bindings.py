"""Bindings for Windows target session identity helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS = (
    "default_windows_session_task_name",
    "desktop_target_contract",
)


def default_windows_session_task_name(bindings: dict, target_name: str) -> str:
    return _binding(bindings, "_windows_target").default_windows_session_task_name(target_name)


def desktop_target_contract(bindings: dict, target_name: str, target: dict) -> dict:
    return _binding(bindings, "_windows_target").desktop_target_contract(target_name, target)


def install_windows_target_session_identity_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
