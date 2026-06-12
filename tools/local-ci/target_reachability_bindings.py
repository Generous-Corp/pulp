"""Bindings from the local_ci facade to target reachability helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import print_binding as _print_binding


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


def utmctl_vm_status(bindings: Mapping[str, Any], vm_name: str) -> str | None:
    return _binding(bindings, "_target_preflight").utmctl_vm_status(
        vm_name,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def utmctl_start(bindings: Mapping[str, Any], vm_name: str) -> bool:
    return _binding(bindings, "_target_preflight").utmctl_start(
        vm_name,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def ensure_host_reachable(bindings: Mapping[str, Any], target_name: str, target_cfg: dict, defaults: dict) -> str | None:
    return _binding(bindings, "_target_preflight").ensure_host_reachable(
        target_name,
        target_cfg,
        defaults,
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
        utmctl_vm_status_fn=_binding(bindings, "utmctl_vm_status"),
        utmctl_start_fn=_binding(bindings, "utmctl_start"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
        print_fn=_print_binding(bindings),
    )


def preflight_target_host_state(bindings: Mapping[str, Any], target_name: str, target_cfg: dict, defaults: dict) -> dict:
    return _binding(bindings, "_target_preflight").preflight_target_host_state(
        target_name,
        target_cfg,
        defaults,
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
    )
