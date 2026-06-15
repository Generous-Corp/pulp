"""Desktop source cache-key and state path helpers."""

from __future__ import annotations

from collections.abc import Callable
import hashlib
import json
from pathlib import Path


def desktop_source_cache_key(source_request: dict) -> str:
    raw = json.dumps(
        {
            "sha": source_request.get("sha"),
            "prepare_command": source_request.get("prepare_command") or "",
        },
        sort_keys=True,
    )
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:12]


def desktop_source_root(
    target_name: str,
    source_request: dict,
    *,
    state_dir_fn: Callable[[], Path],
) -> Path:
    return state_dir_fn() / "desktop-source" / target_name / desktop_source_cache_key(source_request)
