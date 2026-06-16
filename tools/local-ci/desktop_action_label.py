"""Desktop action label helpers."""

from __future__ import annotations

from pathlib import Path
import shlex


def default_desktop_label(command: str | None, *, bundle_id: str | None = None) -> str:
    if bundle_id:
        return bundle_id.split(".")[-1] or bundle_id
    args = shlex.split(command or "")
    if not args:
        return "desktop-run"
    return Path(args[0]).stem or "desktop-run"
