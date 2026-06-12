"""Bindings from the local_ci facade to desktop run manifest and rollup helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


DESKTOP_RUN_ROLLUP_EXPORTS = (
    "desktop_run_manifests",
    "desktop_rollup_dir",
    "write_desktop_run_rollups",
    "prune_desktop_run_manifests",
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
