"""Bindings from the local_ci facade to desktop source-root rewrite helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS = (
    "rewrite_launch_command_for_source_root",
    "rewrite_launch_command_for_posix_root",
    "rewrite_launch_command_for_windows_root",
)


def rewrite_launch_command_for_source_root(
    bindings: Mapping[str, Any],
    command: str | None,
    source_root: Path,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_source_root(
        command,
        source_root,
        root=_binding(bindings, "ROOT"),
    )


def rewrite_launch_command_for_posix_root(
    bindings: Mapping[str, Any],
    command: str | None,
    remote_root: str,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_posix_root(
        command,
        remote_root,
        root=_binding(bindings, "ROOT"),
    )


def rewrite_launch_command_for_windows_root(
    bindings: Mapping[str, Any],
    command: str | None,
    remote_root: str,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_windows_root(
        command,
        remote_root,
        root=_binding(bindings, "ROOT"),
        windows_path_join_fn=_binding(bindings, "windows_path_join"),
    )
