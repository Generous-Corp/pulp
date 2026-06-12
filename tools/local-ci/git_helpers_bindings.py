"""Bindings from the local_ci facade to git/time helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


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
