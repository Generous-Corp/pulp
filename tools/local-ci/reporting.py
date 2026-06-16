"""Compatibility facade for desktop automation report and rollup helpers."""

from __future__ import annotations

from reporting_publish import (
    ARTIFACT_KEYS,
    clear_directory_contents,
    copy_directory_contents,
    publish_report_to_branch,
    slugify_token,
    stage_desktop_publish_report,
)
from reporting_publish_list import desktop_publish_reports, write_desktop_publish_rollups
from reporting_proofs import (
    desktop_manifest_adapter,
    desktop_manifest_run_status,
    desktop_manifest_source,
    desktop_proof_scope_for_adapter,
    desktop_proof_summaries,
    desktop_run_summary,
    normalize_desktop_proof_source_mode,
)
from reporting_run_manifest import desktop_rollup_dir, desktop_run_manifests
from reporting_run_prune import prune_desktop_run_manifests
from reporting_run_rollup import write_desktop_run_rollups


__all__ = [
    "ARTIFACT_KEYS",
    "clear_directory_contents",
    "copy_directory_contents",
    "desktop_manifest_adapter",
    "desktop_manifest_run_status",
    "desktop_manifest_source",
    "desktop_proof_scope_for_adapter",
    "desktop_proof_summaries",
    "desktop_publish_reports",
    "desktop_rollup_dir",
    "desktop_run_manifests",
    "desktop_run_summary",
    "normalize_desktop_proof_source_mode",
    "prune_desktop_run_manifests",
    "publish_report_to_branch",
    "slugify_token",
    "stage_desktop_publish_report",
    "write_desktop_publish_rollups",
    "write_desktop_run_rollups",
]
