"""Desktop automation run pruning selection."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
import time
from typing import Callable


def prune_desktop_run_manifests(
    config: dict,
    *,
    target_name: str | None = None,
    older_than_days: int | None = None,
    keep_last: int | None = None,
    desktop_run_manifests_fn: Callable[..., list[dict]],
) -> list[Path]:
    manifests = desktop_run_manifests_fn(config, target_name=target_name)
    if keep_last is not None:
        manifests = manifests[keep_last:]
    if older_than_days is not None:
        cutoff = time.time() - (older_than_days * 86400)
        filtered: list[dict] = []
        for manifest in manifests:
            completed_at = manifest.get("completed_at") or manifest.get("started_at")
            if not completed_at:
                continue
            try:
                timestamp = datetime.fromisoformat(completed_at.replace("Z", "+00:00")).timestamp()
            except ValueError:
                continue
            if timestamp <= cutoff:
                filtered.append(manifest)
        manifests = filtered
    to_remove: list[Path] = []
    for manifest in manifests:
        bundle_dir = Path(manifest.get("artifacts", {}).get("bundle_dir", "")).expanduser()
        if bundle_dir.is_dir():
            to_remove.append(bundle_dir)
    seen: set[Path] = set()
    ordered: list[Path] = []
    for path in to_remove:
        if path in seen:
            continue
        seen.add(path)
        ordered.append(path)
    return ordered


__all__ = ["prune_desktop_run_manifests"]
