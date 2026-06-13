"""Bindings from the local_ci facade to desktop source command rewrite helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


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
