"""Desktop automation run manifest scanning and rollup paths."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Callable


def desktop_run_manifests(
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    desktop_artifact_root_fn: Callable[[dict], Path],
) -> list[dict]:
    root = desktop_artifact_root_fn(config)
    manifests: list[dict] = []
    target_names = [target_name] if target_name else sorted(p.name for p in root.iterdir() if p.is_dir())
    for target in target_names:
        target_dir = root / target
        if not target_dir.is_dir():
            continue
        action_names = [action] if action else sorted(p.name for p in target_dir.iterdir() if p.is_dir())
        for action_name in action_names:
            action_dir = target_dir / action_name
            if not action_dir.is_dir():
                continue
            for bundle_dir in sorted((p for p in action_dir.iterdir() if p.is_dir()), reverse=True):
                manifest_path = bundle_dir / "manifest.json"
                if not manifest_path.exists():
                    continue
                try:
                    manifest = json.loads(manifest_path.read_text())
                except json.JSONDecodeError:
                    continue
                manifest.setdefault("artifacts", {})
                manifest["artifacts"].setdefault("bundle_dir", str(bundle_dir))
                manifests.append(manifest)
    manifests.sort(key=lambda item: item.get("completed_at") or item.get("started_at") or "", reverse=True)
    return manifests


def desktop_rollup_dir(
    config: dict,
    target_name: str | None = None,
    *,
    desktop_artifact_root_fn: Callable[[dict], Path],
) -> Path:
    root = desktop_artifact_root_fn(config)
    if target_name:
        path = root / target_name
        path.mkdir(parents=True, exist_ok=True)
        return path
    return root


__all__ = ["desktop_rollup_dir", "desktop_run_manifests"]
