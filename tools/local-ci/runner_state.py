"""Runner-info persistence and liveness helpers for local CI."""

from __future__ import annotations

import json
import os
from pathlib import Path

from io_utils import LockBusyError, atomic_write_text, file_lock
from state_paths import drain_lock_path, runner_info_path


def read_runner_info(path: Path | None = None) -> dict | None:
    path = path or runner_info_path()
    if not path.exists():
        return None
    return json.loads(path.read_text())


def pid_alive(pid: int | None) -> bool:
    if not pid or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def current_runner_info(
    *,
    info_path: Path | None = None,
    lock_path: Path | None = None,
    read_runner_info_fn=read_runner_info,
    pid_alive_fn=pid_alive,
    file_lock_fn=file_lock,
) -> dict | None:
    info_path = info_path or runner_info_path()
    lock_path = lock_path or drain_lock_path()
    info = read_runner_info_fn(info_path)
    if not info:
        return None

    if pid_alive_fn(info.get("pid")):
        return info

    try:
        with file_lock_fn(lock_path, blocking=False):
            info_path.unlink(missing_ok=True)
            return None
    except LockBusyError:
        return info


def write_runner_info(info: dict, path: Path | None = None) -> None:
    atomic_write_text(path or runner_info_path(), json.dumps(info, indent=2) + "\n")


def clear_runner_info(path: Path | None = None) -> None:
    (path or runner_info_path()).unlink(missing_ok=True)
