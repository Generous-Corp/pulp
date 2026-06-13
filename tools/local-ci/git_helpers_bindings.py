"""Bindings from the local_ci facade to git/time helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GIT_HELPER_EXPORTS = (
    "now_iso",
    "current_branch",
    "current_sha",
    "git_root_for",
    "resolve_git_ref_sha",
    "short_sha",
)


def now_iso(bindings: Mapping[str, Any]) -> str:
    return _binding(bindings, "_git_helpers").now_iso()


def current_branch(bindings: Mapping[str, Any]) -> str:
    return _binding(bindings, "_git_helpers").current_branch()


def current_sha(bindings: Mapping[str, Any]) -> str:
    return _binding(bindings, "_git_helpers").current_sha()


def git_root_for(bindings: Mapping[str, Any], path: Path) -> Path | None:
    return _binding(bindings, "_git_helpers").git_root_for(path)


def resolve_git_ref_sha(bindings: Mapping[str, Any], ref: str) -> str:
    return _binding(bindings, "_git_helpers").resolve_git_ref_sha(ref)


def short_sha(bindings: Mapping[str, Any], sha: str) -> str:
    return _binding(bindings, "_git_helpers").short_sha(sha)


def install_git_helpers(bindings: dict[str, Any], names: tuple[str, ...] = GIT_HELPER_EXPORTS) -> None:
    known_names = set(GIT_HELPER_EXPORTS)
    git_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), git_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
