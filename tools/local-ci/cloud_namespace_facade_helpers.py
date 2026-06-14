"""Namespace facade dependency wiring helpers for cloud local CI."""

from __future__ import annotations

import subprocess
from collections.abc import Callable


def namespace_instance_duration_secs_with_deps(
    instance: dict,
    *,
    namespace_instance_duration_secs_fn: Callable[..., float | None],
    now_iso_fn: Callable[[], str],
) -> float | None:
    return namespace_instance_duration_secs_fn(instance, now_iso_fn=now_iso_fn)


def normalize_namespace_instance_with_deps(
    instance: dict,
    *,
    normalize_namespace_instance_fn: Callable[..., dict],
    now_iso_fn: Callable[[], str],
) -> dict:
    return normalize_namespace_instance_fn(instance, now_iso_fn=now_iso_fn)


def nsc_available_with_deps(
    *,
    nsc_available_fn: Callable[..., bool],
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None],
) -> bool:
    return nsc_available_fn(nsc_run_fn=nsc_run_fn)


def nsc_version_with_deps(
    *,
    nsc_version_fn: Callable[..., str | None],
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None],
) -> str | None:
    return nsc_version_fn(nsc_run_fn=nsc_run_fn)


def nsc_logged_in_with_deps(
    *,
    nsc_logged_in_fn: Callable[..., bool],
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None],
) -> bool:
    return nsc_logged_in_fn(nsc_run_fn=nsc_run_fn)


def nsc_workspace_info_with_deps(
    *,
    nsc_workspace_info_fn: Callable[..., dict[str, str] | None],
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None],
) -> dict[str, str] | None:
    return nsc_workspace_info_fn(nsc_run_fn=nsc_run_fn)


def nsc_instance_history_with_deps(
    max_entries: int,
    *,
    nsc_instance_history_fn: Callable[..., list[dict]],
    nsc_run_fn: Callable[..., subprocess.CompletedProcess | None],
) -> list[dict]:
    return nsc_instance_history_fn(max_entries=max_entries, nsc_run_fn=nsc_run_fn)


def namespace_instances_for_run_with_deps(
    repository: str,
    run_id: int,
    *,
    namespace_instances_for_run_fn: Callable[..., list[dict]],
    nsc_instance_history_fn: Callable[[], list[dict]],
    normalize_namespace_instance_fn: Callable[[dict], dict],
) -> list[dict]:
    return namespace_instances_for_run_fn(
        repository,
        run_id,
        nsc_instance_history_fn=nsc_instance_history_fn,
        normalize_namespace_instance_fn=normalize_namespace_instance_fn,
    )
