"""Bindings from the local_ci facade to git/time helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_module_attrs


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
    install_module_attrs(bindings, "_git_helpers", names)
