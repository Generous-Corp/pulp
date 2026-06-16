"""Compatibility facade for desktop automation publish report helpers."""

from __future__ import annotations

from reporting_publish_files import clear_directory_contents, copy_directory_contents
from reporting_publish_stage import ARTIFACT_KEYS, slugify_token, stage_desktop_publish_report
from reporting_publish_worktree import publish_report_to_branch


__all__ = [
    "ARTIFACT_KEYS",
    "clear_directory_contents",
    "copy_directory_contents",
    "publish_report_to_branch",
    "slugify_token",
    "stage_desktop_publish_report",
]
