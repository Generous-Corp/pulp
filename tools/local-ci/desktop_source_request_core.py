"""Desktop source request construction helpers."""

from __future__ import annotations

import argparse
from collections.abc import Callable


def make_desktop_source_request(
    args: argparse.Namespace,
    *,
    normalize_desktop_source_mode_fn: Callable[[str | None], str],
    current_branch_fn: Callable[[], str],
    current_sha_fn: Callable[[], str],
) -> dict:
    mode = normalize_desktop_source_mode_fn(getattr(args, "source_mode", "live"))
    return {
        "mode": mode,
        "branch": getattr(args, "branch", None) or current_branch_fn(),
        "sha": getattr(args, "sha", None) or current_sha_fn(),
        "prepare_command": (getattr(args, "prepare_command", None) or "").strip() or None,
        "prepare_timeout_secs": float(getattr(args, "prepare_timeout", 900.0) or 900.0),
    }
