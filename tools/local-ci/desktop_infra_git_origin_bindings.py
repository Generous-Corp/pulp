"""Bindings for desktop git origin URL helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_INFRA_GIT_ORIGIN_EXPORTS = (
    "git_origin_http_url",
    "git_origin_clone_url",
)


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


def install_desktop_infra_git_origin_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_GIT_ORIGIN_EXPORTS,
) -> None:
    known_names = set(DESKTOP_INFRA_GIT_ORIGIN_EXPORTS)
    origin_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), origin_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
