"""Compatibility facade for local-CI cleanup plan bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cleanup_artifact_identity_bindings import (
    CLEANUP_ARTIFACT_IDENTITY_EXPORTS,
    artifact_entry_sort_key,
    install_cleanup_artifact_identity_helpers,
    result_file_job_id,
)
from cleanup_plan_apply_bindings import (
    CLEANUP_PLAN_APPLY_EXPORTS,
    apply_local_ci_cleanup_plan,
    cleanup_plan_lines,
    install_cleanup_plan_apply_helpers,
)
from cleanup_plan_collect_bindings import (
    CLEANUP_PLAN_COLLECT_EXPORTS,
    collect_local_ci_cleanup_plan,
    install_cleanup_plan_collect_helpers,
)


CLEANUP_PLAN_EXPORTS = (
    *CLEANUP_ARTIFACT_IDENTITY_EXPORTS,
    *CLEANUP_PLAN_COLLECT_EXPORTS,
    *CLEANUP_PLAN_APPLY_EXPORTS,
)


def install_cleanup_plan_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_PLAN_EXPORTS,
) -> None:
    known_names = set(CLEANUP_PLAN_EXPORTS)
    identity_names = tuple(name for name in names if name in CLEANUP_ARTIFACT_IDENTITY_EXPORTS)
    collect_names = tuple(name for name in names if name in CLEANUP_PLAN_COLLECT_EXPORTS)
    apply_names = tuple(name for name in names if name in CLEANUP_PLAN_APPLY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cleanup_artifact_identity_helpers(bindings, identity_names)
    install_cleanup_plan_collect_helpers(bindings, collect_names)
    install_cleanup_plan_apply_helpers(bindings, apply_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
