"""Compatibility facade for desktop reporting dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_proof_bindings import (
    DESKTOP_PROOF_EXPORTS,
    desktop_manifest_adapter,
    desktop_manifest_run_status,
    desktop_manifest_source,
    desktop_proof_scope_for_adapter,
    desktop_proof_summaries,
    desktop_run_summary,
    normalize_desktop_proof_source_mode,
)
from desktop_publish_bindings import (
    DESKTOP_PUBLISH_EXPORTS,
    desktop_publish_reports,
    publish_report_to_branch,
    stage_desktop_publish_report,
    write_desktop_publish_rollups,
)
from desktop_run_rollup_bindings import (
    DESKTOP_RUN_ROLLUP_EXPORTS,
    desktop_rollup_dir,
    desktop_run_manifests,
    prune_desktop_run_manifests,
    write_desktop_run_rollups,
)


DESKTOP_REPORTING_EXPORTS = (
    *DESKTOP_PUBLISH_EXPORTS,
    *DESKTOP_RUN_ROLLUP_EXPORTS,
    *DESKTOP_PROOF_EXPORTS,
)


def install_desktop_reporting_helpers(bindings: dict[str, Any], names: tuple[str, ...] = DESKTOP_REPORTING_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
