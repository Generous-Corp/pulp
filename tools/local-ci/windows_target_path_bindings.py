"""Facade dependency bindings for Windows target path helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


WINDOWS_TARGET_PATH_EXPORTS = (
    "windows_path_join",
    "windows_default_repo_checkout_path",
    "windows_repo_path_is_unsafe",
    "update_target_repo_path",
    "windows_repo_checkout_ready",
)


def windows_path_join(bindings: dict, *parts: str) -> str:
    return _binding(bindings, "_windows_target").windows_path_join(*parts)


def windows_default_repo_checkout_path(bindings: dict, home_dir: str | None) -> str:
    return _binding(bindings, "_windows_target").windows_default_repo_checkout_path(home_dir)


def windows_repo_path_is_unsafe(bindings: dict, repo_path: str | None, home_dir: str | None = None) -> bool:
    return _binding(bindings, "_windows_target").windows_repo_path_is_unsafe(repo_path, home_dir)


def update_target_repo_path(bindings: dict, config: dict, target_name: str, repo_path: str) -> None:
    return _binding(bindings, "_windows_target").update_target_repo_path(config, target_name, repo_path)


def windows_repo_checkout_ready(bindings: dict, probe: dict | None) -> bool:
    return _binding(bindings, "_windows_target").windows_repo_checkout_ready(probe)


def install_windows_target_path_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_PATH_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
