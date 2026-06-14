"""Bindings from the local_ci facade to time helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GIT_TIME_HELPER_EXPORTS = ("now_iso",)


def now_iso(bindings: Mapping[str, Any]) -> str:
    return _binding(bindings, "_git_helpers").now_iso()


def install_git_time_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GIT_TIME_HELPER_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
