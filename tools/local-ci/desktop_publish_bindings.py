"""Bindings from the local_ci facade to desktop publish helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


DESKTOP_PUBLISH_EXPORTS = (
    "publish_report_to_branch",
    "stage_desktop_publish_report",
    "desktop_publish_reports",
    "write_desktop_publish_rollups",
)


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
