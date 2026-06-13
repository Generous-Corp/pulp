"""Bindings from the local_ci facade to desktop source command rewrite helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS = (
    "command_path_rewrite_candidate",
    "rewrite_launch_command_for_mapper",
)


def command_path_rewrite_candidate(bindings: Mapping[str, Any], token: str) -> Path | None:
    return _binding(bindings, "_source_prep").command_path_rewrite_candidate(
        token,
        root=_binding(bindings, "ROOT"),
    )


def rewrite_launch_command_for_mapper(
    bindings: Mapping[str, Any],
    command: str | None,
    mapper,
    *,
    windows: bool = False,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_mapper(
        command,
        mapper,
        root=_binding(bindings, "ROOT"),
        windows=windows,
    )


def install_desktop_source_rewrite_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
