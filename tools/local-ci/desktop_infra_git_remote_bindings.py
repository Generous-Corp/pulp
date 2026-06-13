"""Bindings for desktop git remote normalization helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_INFRA_GIT_REMOTE_EXPORTS = (
    "normalize_git_remote_for_http",
    "normalize_git_remote_for_clone",
)


def normalize_git_remote_for_http(bindings: Mapping[str, Any], remote_url: str | None) -> str | None:
    return _binding(bindings, "_git_helpers").normalize_git_remote_for_http(remote_url)


def normalize_git_remote_for_clone(bindings: Mapping[str, Any], remote_url: str | None) -> str | None:
    return _binding(bindings, "_git_helpers").normalize_git_remote_for_clone(remote_url)


def install_desktop_infra_git_remote_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_GIT_REMOTE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
