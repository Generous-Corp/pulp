"""Facade dependency bindings for Windows target constants."""

from __future__ import annotations

from binding_utils import binding as _binding


def windows_required_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_windows_target").WINDOWS_REQUIRED_REMOTE_TOOLS


def windows_optional_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_windows_target").WINDOWS_OPTIONAL_REMOTE_TOOLS


def windows_default_remote_repo_dirname(bindings: dict) -> str:
    return _binding(bindings, "_windows_target").WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME
