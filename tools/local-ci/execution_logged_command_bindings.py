"""Bindings from the local_ci facade to validation logged command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_LOGGED_COMMAND_EXPORTS = ("run_logged_command",)


def run_logged_command(
    bindings: Mapping[str, Any],
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float | None = None,
    stuck_idle_secs: float | None = None,
) -> dict:
    execution = _binding(bindings, "_execution")
    return execution.run_logged_command(
        cmd,
        cwd=cwd,
        input_text=input_text,
        timeout=timeout,
        log_path=log_path,
        report_progress=report_progress,
        heartbeat_interval_secs=execution.HEARTBEAT_INTERVAL_SECS
        if heartbeat_interval_secs is None
        else heartbeat_interval_secs,
        stuck_idle_secs=execution.STUCK_IDLE_SECS if stuck_idle_secs is None else stuck_idle_secs,
    )


def install_execution_logged_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_LOGGED_COMMAND_EXPORTS,
) -> None:
    known_names = set(EXECUTION_LOGGED_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
