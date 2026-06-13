"""Bindings for desktop git command execution helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_INFRA_GIT_RUN_EXPORTS = ("run_git",)


def run_git(bindings: Mapping[str, Any], args: list[str], *, cwd: Path, check: bool = True):
    return _binding(bindings, "_git_helpers").run_git(
        args,
        cwd=cwd,
        check=check,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_desktop_infra_git_run_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_GIT_RUN_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
