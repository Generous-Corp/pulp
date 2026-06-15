"""Desktop automation publish report listing and rollups."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Callable


def desktop_publish_reports(
    config: dict,
    *,
    limit: int | None = None,
    desktop_publish_root_fn: Callable[[dict], Path],
) -> list[dict]:
    root = desktop_publish_root_fn(config)
    reports: list[dict] = []
    for publish_dir in sorted((p for p in root.iterdir() if p.is_dir()), reverse=True):
        index_json = publish_dir / "index.json"
        index_html = publish_dir / "index.html"
        if not index_json.exists():
            continue
        try:
            payload = json.loads(index_json.read_text())
        except json.JSONDecodeError:
            continue
        payload["output_dir"] = str(publish_dir)
        payload.setdefault("index_json", str(index_json))
        payload.setdefault("index_html", str(index_html))
        reports.append(payload)
    reports.sort(key=lambda item: item.get("generated_at") or "", reverse=True)
    if limit is not None:
        reports = reports[:limit]
    return reports


def write_desktop_publish_rollups(
    config: dict,
    *,
    desktop_publish_root_fn: Callable[[dict], Path],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    atomic_write_text_fn: Callable[[Path, str], None],
) -> None:
    root = desktop_publish_root_fn(config)
    reports = desktop_publish_reports_fn(config)
    latest_report = reports[0] if reports else None
    atomic_write_text_fn(root / "latest-report.json", json.dumps(latest_report, indent=2) + "\n")
    reports_jsonl = "".join(json.dumps(report, sort_keys=True) + "\n" for report in reports)
    atomic_write_text_fn(root / "reports.jsonl", reports_jsonl)


__all__ = ["desktop_publish_reports", "write_desktop_publish_rollups"]
