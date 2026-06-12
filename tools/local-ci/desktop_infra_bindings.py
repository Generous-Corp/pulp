"""Facade dependency bindings for desktop infrastructure helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


def normalize_git_remote_for_http(bindings: Mapping[str, Any], remote_url: str | None) -> str | None:
    return _binding(bindings, "_git_helpers").normalize_git_remote_for_http(remote_url)


def normalize_git_remote_for_clone(bindings: Mapping[str, Any], remote_url: str | None) -> str | None:
    return _binding(bindings, "_git_helpers").normalize_git_remote_for_clone(remote_url)


def git_origin_http_url(bindings: Mapping[str, Any], repo_root: Path) -> str | None:
    return _binding(bindings, "_git_helpers").git_origin_http_url(
        repo_root,
        run_fn=_binding(bindings, "subprocess").run,
    )


def git_origin_clone_url(bindings: Mapping[str, Any], repo_root: Path) -> str | None:
    return _binding(bindings, "_git_helpers").git_origin_clone_url(
        repo_root,
        run_fn=_binding(bindings, "subprocess").run,
    )


def clear_directory_contents(bindings: Mapping[str, Any], path: Path) -> None:
    return _binding(bindings, "_reporting").clear_directory_contents(path)


def copy_directory_contents(bindings: Mapping[str, Any], src: Path, dest: Path) -> None:
    return _binding(bindings, "_reporting").copy_directory_contents(src, dest)


def run_git(bindings: Mapping[str, Any], args: list[str], *, cwd: Path, check: bool = True):
    return _binding(bindings, "_git_helpers").run_git(
        args,
        cwd=cwd,
        check=check,
        run_fn=_binding(bindings, "subprocess").run,
    )


def slugify_token(bindings: Mapping[str, Any], value: str, *, max_len: int = 48) -> str:
    return _binding(bindings, "_reporting").slugify_token(value, max_len=max_len)


def wait_for_path(bindings: Mapping[str, Any], path: Path, timeout_secs: float) -> Path:
    return _binding(bindings, "_io_utils").wait_for_path(path, timeout_secs)
