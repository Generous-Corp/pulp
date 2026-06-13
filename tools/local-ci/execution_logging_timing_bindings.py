"""Bindings from the local_ci facade to validation logging timing constants."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_LOGGING_TIMING_EXPORTS = (
    "heartbeat_interval_secs",
    "stuck_idle_secs",
)


def heartbeat_interval_secs(bindings: Mapping[str, Any]) -> float:
    return _binding(bindings, "_execution").HEARTBEAT_INTERVAL_SECS


def stuck_idle_secs(bindings: Mapping[str, Any]) -> float:
    return _binding(bindings, "_execution").STUCK_IDLE_SECS


def install_execution_logging_timing_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_LOGGING_TIMING_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
