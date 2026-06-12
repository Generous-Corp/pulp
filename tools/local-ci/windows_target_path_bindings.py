"""Facade dependency bindings for Windows target path helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


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
