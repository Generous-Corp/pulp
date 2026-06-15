"""Desktop automation run rollup writing."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Callable


def write_desktop_run_rollups(
    config: dict,
    *,
    target_name: str | None = None,
    desktop_rollup_dir_fn: Callable[..., Path],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    atomic_write_text_fn: Callable[[Path, str], None],
) -> None:
    rollup_dir = desktop_rollup_dir_fn(config, target_name)
    manifests = desktop_run_manifests_fn(config, target_name=target_name)
    summaries = [desktop_run_summary_fn(config, manifest) for manifest in manifests]
    latest_run = summaries[0] if summaries else None
    latest_proof_matches = desktop_proof_summaries_fn(config, target_name=target_name, limit=1)
    latest_proof = latest_proof_matches[0] if latest_proof_matches else None
    atomic_write_text_fn(rollup_dir / "latest-run.json", json.dumps(latest_run, indent=2) + "\n")
    atomic_write_text_fn(rollup_dir / "latest-proof.json", json.dumps(latest_proof, indent=2) + "\n")
    jsonl_payload = "".join(json.dumps(summary, sort_keys=True) + "\n" for summary in summaries)
    atomic_write_text_fn(rollup_dir / "runs.jsonl", jsonl_payload)


__all__ = ["write_desktop_run_rollups"]
