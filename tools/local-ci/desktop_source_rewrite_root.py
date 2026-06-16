"""Desktop launch command source-root rewrite helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from desktop_source_rewrite_command import rewrite_launch_command_for_mapper


def rewrite_launch_command_for_source_root(command: str | None, source_root: Path, *, root: Path) -> str | None:
    return rewrite_launch_command_for_mapper(command, lambda rel: str(source_root / rel), root=root)


def rewrite_launch_command_for_posix_root(command: str | None, remote_root: str, *, root: Path) -> str | None:
    return rewrite_launch_command_for_mapper(command, lambda rel: f"{remote_root}/{rel.as_posix()}", root=root)


def rewrite_launch_command_for_windows_root(
    command: str | None,
    remote_root: str,
    *,
    root: Path,
    windows_path_join_fn: Callable[..., str],
) -> str | None:
    return rewrite_launch_command_for_mapper(
        command,
        lambda rel: windows_path_join_fn(remote_root, str(rel).replace("/", "\\")),
        root=root,
        windows=True,
    )
