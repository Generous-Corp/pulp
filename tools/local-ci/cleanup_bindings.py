"""Compatibility facade for local_ci cleanup dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cleanup_plan_bindings import (
    CLEANUP_PLAN_EXPORTS,
    apply_local_ci_cleanup_plan,
    artifact_entry_sort_key,
    cleanup_plan_lines,
    collect_local_ci_cleanup_plan,
    install_cleanup_plan_helpers,
    result_file_job_id,
)
from cleanup_stale_windows_bindings import (
    CLEANUP_STALE_WINDOWS_EXPORTS,
    cleanup_stale_windows_validator,
    collect_stale_windows_cleanup_candidates_unlocked,
    install_cleanup_stale_windows_helpers,
)


CLEANUP_EXPORTS = (
    *CLEANUP_PLAN_EXPORTS,
    *CLEANUP_STALE_WINDOWS_EXPORTS,
)


def install_cleanup_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLEANUP_EXPORTS) -> None:
    plan_names = tuple(name for name in names if name in CLEANUP_PLAN_EXPORTS)
    stale_names = tuple(name for name in names if name in CLEANUP_STALE_WINDOWS_EXPORTS)
    known_names = set(CLEANUP_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cleanup_plan_helpers(bindings, plan_names)
    install_cleanup_stale_windows_helpers(bindings, stale_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
