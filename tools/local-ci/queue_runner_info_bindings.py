"""Bindings from the local_ci facade to queue runner-info helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_RUNNER_INFO_EXPORTS = (
    "read_runner_info",
    "pid_alive",
    "current_runner_info",
    "write_runner_info",
    "clear_runner_info",
)


def read_runner_info(bindings: Mapping[str, Any]) -> dict | None:
    return _binding(bindings, "_runner_state").read_runner_info()


def pid_alive(bindings: Mapping[str, Any], pid: int | None) -> bool:
    return _binding(bindings, "_runner_state").pid_alive(pid)


def current_runner_info(bindings: Mapping[str, Any]) -> dict | None:
    return _binding(bindings, "_runner_state").current_runner_info()


def write_runner_info(bindings: Mapping[str, Any], info: dict) -> None:
    _binding(bindings, "_runner_state").write_runner_info(info)


def clear_runner_info(bindings: Mapping[str, Any]) -> None:
    _binding(bindings, "_runner_state").clear_runner_info()
