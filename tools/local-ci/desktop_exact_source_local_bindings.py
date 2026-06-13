"""Bindings from the local_ci facade to local exact-source worktree helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def local_worktree_matches(bindings: Mapping[str, Any], path: Path, sha: str) -> bool:
    return _binding(bindings, "_source_prep").local_worktree_matches(
        path,
        sha,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def reset_local_worktree(bindings: Mapping[str, Any], path: Path) -> None:
    return _binding(bindings, "_source_prep").reset_local_worktree(
        path,
        root=_binding(bindings, "ROOT"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )
