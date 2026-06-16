"""Compatibility facade for cloud dependency wiring helpers."""

from __future__ import annotations

from cloud_billing_facade_helpers import (
    enrich_cloud_record_provider_metadata_with_deps,
    estimate_billing_period_totals_with_deps,
    fetch_github_repo_actions_billing_summary_with_deps,
)
from cloud_namespace_facade_helpers import (
    namespace_instance_duration_secs_with_deps,
    namespace_instances_for_run_with_deps,
    normalize_namespace_instance_with_deps,
    nsc_available_with_deps,
    nsc_instance_history_with_deps,
    nsc_logged_in_with_deps,
    nsc_version_with_deps,
    nsc_workspace_info_with_deps,
)
from cloud_record_facade_helpers import (
    cloud_record_summary_with_deps,
    list_cloud_records_with_deps,
    save_cloud_record_with_deps,
)
from cloud_refresh_facade_helpers import (
    refresh_cloud_record_with_deps,
    resolve_github_repository_with_deps,
    update_cloud_record_from_run_with_deps,
)


__all__ = (
    "cloud_record_summary_with_deps",
    "enrich_cloud_record_provider_metadata_with_deps",
    "estimate_billing_period_totals_with_deps",
    "fetch_github_repo_actions_billing_summary_with_deps",
    "list_cloud_records_with_deps",
    "namespace_instance_duration_secs_with_deps",
    "namespace_instances_for_run_with_deps",
    "normalize_namespace_instance_with_deps",
    "nsc_available_with_deps",
    "nsc_instance_history_with_deps",
    "nsc_logged_in_with_deps",
    "nsc_version_with_deps",
    "nsc_workspace_info_with_deps",
    "refresh_cloud_record_with_deps",
    "resolve_github_repository_with_deps",
    "save_cloud_record_with_deps",
    "update_cloud_record_from_run_with_deps",
)
