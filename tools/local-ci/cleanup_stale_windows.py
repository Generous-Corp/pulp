"""Compatibility facade for stale Windows validator cleanup helpers."""

from __future__ import annotations

from cleanup_stale_windows_candidates import collect_stale_windows_cleanup_candidates_unlocked
from cleanup_stale_windows_reclaim import reclaim_stale_remote_validator_candidates
from cleanup_stale_windows_remote import cleanup_stale_windows_validator, stale_windows_validator_cleanup_script
from cleanup_stale_windows_result import (
    stale_windows_validator_cleanup_status,
    stale_windows_validator_update_fields,
)


__all__ = [
    "cleanup_stale_windows_validator",
    "collect_stale_windows_cleanup_candidates_unlocked",
    "reclaim_stale_remote_validator_candidates",
    "stale_windows_validator_cleanup_script",
    "stale_windows_validator_cleanup_status",
    "stale_windows_validator_update_fields",
]
