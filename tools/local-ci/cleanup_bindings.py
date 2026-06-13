"""Compatibility facade for local_ci cleanup dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cleanup_plan_bindings import (
    apply_local_ci_cleanup_plan,
    artifact_entry_sort_key,
    cleanup_plan_lines,
    collect_local_ci_cleanup_plan,
    result_file_job_id,
)
from cleanup_stale_windows_bindings import (
    cleanup_stale_windows_validator,
    collect_stale_windows_cleanup_candidates_unlocked,
)


CLEANUP_EXPORTS = (
    "result_file_job_id",
    "artifact_entry_sort_key",
    "collect_local_ci_cleanup_plan",
    "apply_local_ci_cleanup_plan",
    "cleanup_plan_lines",
    "collect_stale_windows_cleanup_candidates_unlocked",
    "cleanup_stale_windows_validator",
)


def install_cleanup_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLEANUP_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
