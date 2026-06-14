"""Compatibility facade for validation runner helpers."""

from __future__ import annotations

from validation_runner_local import run_local_validation
from validation_runner_posix import run_posix_ssh_validation
from validation_runner_windows import run_windows_ssh_validation


__all__ = (
    "run_local_validation",
    "run_posix_ssh_validation",
    "run_windows_ssh_validation",
)
