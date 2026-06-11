"""Bindings from the local_ci facade to desktop reporting helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def publish_report_to_branch(bindings: Mapping[str, Any], config: dict, report: dict) -> dict:
    return _binding(bindings, "_reporting").publish_report_to_branch(
        config,
        report,
        root=_binding(bindings, "ROOT"),
        run_git_fn=_binding(bindings, "_run_git"),
        reset_local_worktree_fn=_binding(bindings, "_reset_local_worktree"),
        clear_directory_contents_fn=_binding(bindings, "_clear_directory_contents"),
        git_origin_http_url_fn=_binding(bindings, "git_origin_http_url"),
    )


def stage_desktop_publish_report(
    bindings: Mapping[str, Any],
    config: dict,
    manifests: list[dict],
    *,
    output_dir: Path | None = None,
    label: str | None = None,
) -> dict:
    return _binding(bindings, "_reporting").stage_desktop_publish_report(
        config,
        manifests,
        output_dir=output_dir,
        label=label,
        create_desktop_publish_bundle_fn=_binding(bindings, "create_desktop_publish_bundle"),
        now_iso_fn=_binding(bindings, "now_iso"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        write_desktop_publish_rollups_fn=_binding(bindings, "write_desktop_publish_rollups"),
        publish_report_to_branch_fn=_binding(bindings, "publish_report_to_branch"),
    )


def desktop_publish_reports(bindings: Mapping[str, Any], config: dict, *, limit: int | None = None) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_publish_reports(
        config,
        limit=limit,
        desktop_publish_root_fn=_binding(bindings, "desktop_publish_root"),
    )


def write_desktop_publish_rollups(bindings: Mapping[str, Any], config: dict) -> None:
    return _binding(bindings, "_reporting").write_desktop_publish_rollups(
        config,
        desktop_publish_root_fn=_binding(bindings, "desktop_publish_root"),
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def desktop_run_manifests(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_run_manifests(
        config,
        target_name=target_name,
        action=action,
        desktop_artifact_root_fn=_binding(bindings, "desktop_artifact_root"),
    )


def desktop_proof_summaries(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    source_mode: str | None = None,
    sha: str | None = None,
    branch: str | None = None,
    limit: int | None = None,
) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_proof_summaries(
        config,
        target_name=target_name,
        action=action,
        source_mode=source_mode,
        sha=sha,
        branch=branch,
        limit=limit,
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
    )


def desktop_rollup_dir(bindings: Mapping[str, Any], config: dict, target_name: str | None = None) -> Path:
    return _binding(bindings, "_reporting").desktop_rollup_dir(
        config,
        target_name,
        desktop_artifact_root_fn=_binding(bindings, "desktop_artifact_root"),
    )


def write_desktop_run_rollups(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
) -> None:
    return _binding(bindings, "_reporting").write_desktop_run_rollups(
        config,
        target_name=target_name,
        desktop_rollup_dir_fn=_binding(bindings, "desktop_rollup_dir"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def prune_desktop_run_manifests(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    older_than_days: int | None = None,
    keep_last: int | None = None,
) -> list[Path]:
    return _binding(bindings, "_reporting").prune_desktop_run_manifests(
        config,
        target_name=target_name,
        older_than_days=older_than_days,
        keep_last=keep_last,
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
    )
