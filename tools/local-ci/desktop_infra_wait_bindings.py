"""Bindings from the local_ci facade to desktop wait helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_INFRA_WAIT_EXPORTS = ("wait_for_path",)


def wait_for_path(bindings: Mapping[str, Any], path: Path, timeout_secs: float) -> Path:
    return _binding(bindings, "_io_utils").wait_for_path(path, timeout_secs)


def install_desktop_infra_wait_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_WAIT_EXPORTS,
) -> None:
    known_names = set(DESKTOP_INFRA_WAIT_EXPORTS)
    wait_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), wait_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
