"""Cloud record facade dependency wiring helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def save_cloud_record_with_deps(
    record: dict,
    *,
    save_cloud_record_fn: Callable[..., Path],
    ensure_state_dirs_fn: Callable[[], None],
    cloud_run_path_fn: Callable[[str], Path],
    atomic_write_text_fn: Callable[[Path, str], None],
) -> Path:
    return save_cloud_record_fn(
        record,
        ensure_state_dirs_fn=ensure_state_dirs_fn,
        cloud_run_path_fn=cloud_run_path_fn,
        atomic_write_text_fn=atomic_write_text_fn,
    )


def list_cloud_records_with_deps(
    *,
    limit: int | None,
    list_cloud_records_fn: Callable[..., list[dict]],
    ensure_state_dirs_fn: Callable[[], None],
    cloud_runs_dir_fn: Callable[[], Path],
    load_cloud_record_fn: Callable[[Path], dict],
) -> list[dict]:
    return list_cloud_records_fn(
        limit=limit,
        ensure_state_dirs_fn=ensure_state_dirs_fn,
        cloud_runs_dir_fn=cloud_runs_dir_fn,
        load_cloud_record_fn=load_cloud_record_fn,
    )


def cloud_record_summary_with_deps(
    record: dict,
    config: dict | None,
    *,
    cloud_record_summary_fn: Callable[..., str],
    estimate_cloud_record_cost_fn: Callable[[dict, dict | None], dict],
    format_currency_amount_fn: Callable[..., str],
) -> str:
    return cloud_record_summary_fn(
        record,
        config,
        estimate_cloud_record_cost_fn=estimate_cloud_record_cost_fn,
        format_currency_amount_fn=format_currency_amount_fn,
    )
