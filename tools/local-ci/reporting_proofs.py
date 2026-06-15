"""Compatibility facade for desktop proof and run-summary helpers."""

from __future__ import annotations

from reporting_proof_list import desktop_proof_summaries
from reporting_proof_source import desktop_manifest_source, normalize_desktop_proof_source_mode
from reporting_run_summary import (
    desktop_manifest_adapter,
    desktop_manifest_run_status,
    desktop_proof_scope_for_adapter,
    desktop_run_summary,
)


__all__ = [
    "desktop_manifest_adapter",
    "desktop_manifest_run_status",
    "desktop_manifest_source",
    "desktop_proof_scope_for_adapter",
    "desktop_proof_summaries",
    "desktop_run_summary",
    "normalize_desktop_proof_source_mode",
]
