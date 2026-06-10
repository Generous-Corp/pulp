"""Runner-info persistence and liveness helpers for local CI."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Callable

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


def stale_running_jobs_for_current_runner(
    queue: list[dict],
    *,
    stale_running_jobs_for_runner_unlocked_fn: Callable[[list[dict], int | None], list[dict]],
    read_runner_info_fn: Callable[[], dict | None] = read_runner_info,
    pid_alive_fn: Callable[[int | None], bool] = pid_alive,
    clear_runner_info_fn: Callable[[], None] = clear_runner_info,
) -> list[dict]:
    runner = read_runner_info_fn()
    runner_pid = runner.get("pid") if runner else None
    runner_alive = pid_alive_fn(runner_pid)

    if runner and not runner_alive:
        clear_runner_info_fn()
        runner = None
        runner_pid = None

    return stale_running_jobs_for_runner_unlocked_fn(
        queue,
        runner_pid if runner else None,
    )
