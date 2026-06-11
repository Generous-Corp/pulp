"""Desktop automation artifact path helpers for local CI."""

from __future__ import annotations

from datetime import datetime
import json
from pathlib import Path
from typing import Callable
import uuid


def desktop_target_receipt_path(
    target_name: str,
    *,
    desktop_receipts_dir_fn: Callable[[], Path],
) -> Path:
    return desktop_receipts_dir_fn() / f"{target_name}.json"


def desktop_receipt_for(
    target_name: str,
    *,
    desktop_target_receipt_path_fn: Callable[[str], Path],
) -> dict | None:
    path = desktop_target_receipt_path_fn(target_name)
    if not path.exists():
        return None
    return json.loads(path.read_text())


def desktop_artifact_root(config: dict) -> Path:
    path = Path(config["desktop_automation"]["artifact_root"]).expanduser()
    path.mkdir(parents=True, exist_ok=True)
    return path


def create_desktop_run_bundle(
    config: dict,
    target_name: str,
    action: str,
    *,
    now_fn: Callable[[], datetime] = datetime.now,
    uuid_hex_fn: Callable[[], str] | None = None,
) -> Path:
    ts = now_fn().strftime("%Y%m%d-%H%M%S")
    run_id = (uuid_hex_fn or (lambda: uuid.uuid4().hex))()[:8]
    path = desktop_artifact_root(config) / target_name / action / f"{ts}-{run_id}"
    (path / "screenshots").mkdir(parents=True, exist_ok=True)
    return path


def desktop_publish_root(config: dict) -> Path:
    path = desktop_artifact_root(config) / "_published"
    path.mkdir(parents=True, exist_ok=True)
    return path


def create_desktop_publish_bundle(
    config: dict,
    *,
    now_fn: Callable[[], datetime] = datetime.now,
    uuid_hex_fn: Callable[[], str] | None = None,
) -> Path:
    ts = now_fn().strftime("%Y%m%d-%H%M%S")
    run_id = (uuid_hex_fn or (lambda: uuid.uuid4().hex))()[:8]
    path = desktop_publish_root(config) / f"{ts}-{run_id}"
    (path / "assets").mkdir(parents=True, exist_ok=True)
    return path
