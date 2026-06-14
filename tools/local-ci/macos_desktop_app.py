"""macOS app bundle helper functions for local CI."""

from __future__ import annotations

from pathlib import Path
import plistlib
import shlex


def detect_macos_app_bundle(command: str | None) -> Path | None:
    if not command:
        return None
    args = shlex.split(command)
    if not args:
        return None
    exec_path = Path(args[0]).expanduser()
    candidates = [exec_path, *exec_path.parents]
    for candidate in candidates:
        if candidate.suffix == ".app":
            return candidate
    return None


def macos_bundle_id_for_app_path(app_path: Path) -> str | None:
    info_plist = app_path / "Contents" / "Info.plist"
    if not info_plist.exists():
        return None
    try:
        payload = plistlib.loads(info_plist.read_bytes())
    except (plistlib.InvalidFileException, OSError):
        return None
    bundle_id = payload.get("CFBundleIdentifier")
    return bundle_id if isinstance(bundle_id, str) and bundle_id else None


def macos_window_probe_path(script_dir: Path) -> Path:
    return script_dir / "macos_window_probe.swift"
