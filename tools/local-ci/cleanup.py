"""Compatibility surface for local CI cleanup helpers.

cleanup_bindings.py installs the historical `local_ci.*` facade names so
existing callers can keep using and monkey-patching those seams.
"""

from __future__ import annotations

from cleanup_artifacts import (
    DEFAULT_KEEP_COMPLETED_JOBS,
    apply_local_ci_cleanup_plan,
    artifact_entry_sort_key,
    cleanup_plan_lines,
    collect_local_ci_cleanup_plan,
    result_file_job_id,
)
from cleanup_stale_windows import (
    cleanup_stale_windows_validator,
    collect_stale_windows_cleanup_candidates_unlocked,
    reclaim_stale_remote_validator_candidates,
    stale_windows_validator_cleanup_script,
    stale_windows_validator_cleanup_status,
    stale_windows_validator_update_fields,
)
