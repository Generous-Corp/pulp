"""Bindings from the local_ci facade to desktop git infrastructure helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_INFRA_GIT_EXPORTS = (
    "normalize_git_remote_for_http",
    "normalize_git_remote_for_clone",
    "git_origin_http_url",
    "git_origin_clone_url",
    "run_git",
)


def normalize_git_remote_for_http(bindings: Mapping[str, Any], remote_url: str | None) -> str | None:
    return _binding(bindings, "_git_helpers").normalize_git_remote_for_http(remote_url)


def normalize_git_remote_for_clone(bindings: Mapping[str, Any], remote_url: str | None) -> str | None:
    return _binding(bindings, "_git_helpers").normalize_git_remote_for_clone(remote_url)


def git_origin_http_url(bindings: Mapping[str, Any], repo_root: Path) -> str | None:
    return _binding(bindings, "_git_helpers").git_origin_http_url(
        repo_root,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def git_origin_clone_url(bindings: Mapping[str, Any], repo_root: Path) -> str | None:
    return _binding(bindings, "_git_helpers").git_origin_clone_url(
        repo_root,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def run_git(bindings: Mapping[str, Any], args: list[str], *, cwd: Path, check: bool = True):
    return _binding(bindings, "_git_helpers").run_git(
        args,
        cwd=cwd,
        check=check,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_desktop_infra_git_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_GIT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
