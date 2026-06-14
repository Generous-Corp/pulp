"""Bindings for desktop git command execution helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from desktop_infra_git_run_dependency_bindings import run_git_dependencies


DESKTOP_INFRA_GIT_RUN_EXPORTS = ("run_git",)


def run_git(bindings: Mapping[str, Any], args: list[str], *, cwd: Path, check: bool = True):
    return _binding(bindings, "_git_helpers").run_git(
        args,
        cwd=cwd,
        check=check,
        **run_git_dependencies(bindings),
    )


def install_desktop_infra_git_run_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_GIT_RUN_EXPORTS,
) -> None:
    known_names = set(DESKTOP_INFRA_GIT_RUN_EXPORTS)
    run_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
