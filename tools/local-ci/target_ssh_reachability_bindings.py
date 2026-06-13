"""Bindings from the local_ci facade to SSH target reachability helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


TARGET_SSH_REACHABILITY_EXPORTS = (
    "ssh_probe",
    "ssh_reachable",
    "ssh_failure_detail",
    "ssh_command_result",
)


def ssh_probe(bindings: Mapping[str, Any], host: str, timeout: int = 5) -> Any:
    return _binding(bindings, "_target_preflight").ssh_probe(
        host,
        timeout,
        run_ssh_subprocess_fn=_binding(bindings, "run_ssh_subprocess"),
    )


def ssh_reachable(bindings: Mapping[str, Any], host: str, timeout: int = 5) -> bool:
    return _binding(bindings, "_target_preflight").ssh_reachable(
        host,
        timeout,
        ssh_probe_fn=_binding(bindings, "ssh_probe"),
    )


def ssh_failure_detail(bindings: Mapping[str, Any], host: str, timeout: int = 5) -> str:
    return _binding(bindings, "_target_preflight").ssh_failure_detail(
        host,
        timeout,
        ssh_probe_fn=_binding(bindings, "ssh_probe"),
    )


def ssh_command_result(bindings: Mapping[str, Any], host: str, remote_cmd: str, *, timeout: int = 30) -> Any:
    return _binding(bindings, "_target_preflight").ssh_command_result(
        host,
        remote_cmd,
        timeout=timeout,
        run_ssh_subprocess_fn=_binding(bindings, "run_ssh_subprocess"),
    )
