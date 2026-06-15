"""Compatibility facade for Windows remote repository checkout helpers."""
from __future__ import annotations

from windows_repo_checkout_ensure import ensure_windows_remote_repo_checkout
from windows_repo_checkout_probe import probe_windows_repo_checkout


__all__ = [
    "ensure_windows_remote_repo_checkout",
    "probe_windows_repo_checkout",
]
