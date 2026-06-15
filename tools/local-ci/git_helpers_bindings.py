"""Compatibility composer for local_ci git/time helper bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from git_ref_helpers_bindings import (
    GIT_REF_HELPER_EXPORTS,
    current_branch,
    current_sha,
    git_root_for,
    install_git_ref_helpers,
    resolve_git_ref_sha,
    short_sha,
)
from git_time_helpers_bindings import GIT_TIME_HELPER_EXPORTS, install_git_time_helpers, now_iso


GIT_HELPER_EXPORTS = GIT_TIME_HELPER_EXPORTS + GIT_REF_HELPER_EXPORTS


def install_git_helpers(bindings: dict[str, Any], names: tuple[str, ...] = GIT_HELPER_EXPORTS) -> None:
    time_names = tuple(name for name in names if name in GIT_TIME_HELPER_EXPORTS)
    ref_names = tuple(name for name in names if name in GIT_REF_HELPER_EXPORTS)
    known_names = set(GIT_TIME_HELPER_EXPORTS) | set(GIT_REF_HELPER_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_git_time_helpers(bindings, time_names)
    install_git_ref_helpers(bindings, ref_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
