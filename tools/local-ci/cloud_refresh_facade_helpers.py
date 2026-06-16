"""Cloud repository and refresh facade dependency wiring helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def resolve_github_repository_with_deps(
    settings: dict,
    *,
    gh_repo_name_fn: Callable[[], str | None],
) -> str:
    repository = settings.get("repository", "").strip()
    if repository:
        return repository
    discovered = gh_repo_name_fn()
    if discovered:
        return discovered
    raise ValueError(
        "Could not determine GitHub repository. Set github_actions.repository in tools/local-ci/config.json "
        "or make sure `gh repo view` works in this checkout."
    )


def update_cloud_record_from_run_with_deps(
    record: dict,
    snapshot: dict,
    *,
    provider_resolved: str | None,
    update_cloud_record_from_run_fn: Callable[..., dict],
    now_iso_fn: Callable[[], str],
) -> dict:
    return update_cloud_record_from_run_fn(
        record,
        snapshot,
        provider_resolved=provider_resolved,
        now_iso_fn=now_iso_fn,
    )


def refresh_cloud_record_with_deps(
    record: dict,
    repository: str,
    *,
    require_snapshot: bool,
    normalize_cloud_record_fn: Callable[[dict], dict],
    gh_run_view_fn: Callable[[str, int], dict | None],
    update_cloud_record_from_run_fn: Callable[[dict, dict], dict],
    enrich_cloud_record_provider_metadata_fn: Callable[[dict], dict],
    save_cloud_record_fn: Callable[[dict], Path],
) -> dict:
    run_id = record.get("run_id")
    if not run_id:
        return normalize_cloud_record_fn(record)
    snapshot = gh_run_view_fn(repository, int(run_id))
    if not snapshot:
        if require_snapshot:
            raise RuntimeError(f"Failed to refresh GitHub run {run_id} from {repository}.")
        return normalize_cloud_record_fn(record)
    refreshed = enrich_cloud_record_provider_metadata_fn(update_cloud_record_from_run_fn(record, snapshot))
    save_cloud_record_fn(refreshed)
    return refreshed
