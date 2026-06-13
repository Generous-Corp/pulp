"""Facade dependency bindings for Windows target constants."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


WINDOWS_TARGET_CONSTANT_EXPORTS = (
    "windows_required_remote_tools",
    "windows_optional_remote_tools",
    "windows_default_remote_repo_dirname",
)


def windows_required_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_windows_target").WINDOWS_REQUIRED_REMOTE_TOOLS


def windows_optional_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_windows_target").WINDOWS_OPTIONAL_REMOTE_TOOLS


def windows_default_remote_repo_dirname(bindings: dict) -> str:
    return _binding(bindings, "_windows_target").WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME


def install_windows_target_constant_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_CONSTANT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
