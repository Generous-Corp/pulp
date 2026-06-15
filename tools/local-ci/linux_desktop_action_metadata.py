"""Linux desktop action manifest metadata helpers."""

from __future__ import annotations

from pathlib import Path


def read_linux_pid_file(pid_path: Path) -> int | None:
    if not pid_path.exists():
        return None
    try:
        return int(pid_path.read_text().strip())
    except ValueError:
        return None


def attach_linux_window_metadata(
    manifest: dict,
    *,
    window_id_path: Path,
    window_title_path: Path,
) -> None:
    if not (window_id_path.exists() or window_title_path.exists()):
        return
    manifest["window"] = {}
    if window_id_path.exists():
        manifest["window"]["window_id"] = window_id_path.read_text().strip()
    if window_title_path.exists():
        manifest["window"]["title"] = window_title_path.read_text().strip()
