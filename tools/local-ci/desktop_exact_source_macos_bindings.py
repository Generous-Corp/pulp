"""Bindings from the local_ci facade to macOS exact-source preparation helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_EXACT_SOURCE_MACOS_EXPORTS = ("prepare_macos_exact_sha_source",)


def prepare_macos_exact_sha_source(
    bindings: Mapping[str, Any],
    bundle_dir: Path,
    target_name: str,
    command: str,
    source_request: dict,
) -> dict:
    return _binding(bindings, "_source_prep").prepare_macos_exact_sha_source(
        bundle_dir,
        target_name,
        command,
        source_request,
        root=_binding(bindings, "ROOT"),
        desktop_source_root_fn=_binding(bindings, "desktop_source_root"),
        local_worktree_matches_fn=_binding(bindings, "_local_worktree_matches"),
        reset_local_worktree_fn=_binding(bindings, "_reset_local_worktree"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        tail_lines_fn=_binding(bindings, "tail_lines"),
        rewrite_launch_command_for_source_root_fn=_binding(bindings, "rewrite_launch_command_for_source_root"),
    )


def install_desktop_exact_source_macos_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_EXACT_SOURCE_MACOS_EXPORTS,
) -> None:
    known_names = set(DESKTOP_EXACT_SOURCE_MACOS_EXPORTS)
    macos_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), macos_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
