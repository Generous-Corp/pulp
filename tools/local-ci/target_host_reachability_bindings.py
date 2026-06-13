"""Bindings from the local_ci facade to host reachability orchestration helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import print_binding as _print_binding


TARGET_HOST_REACHABILITY_EXPORTS = (
    "ensure_host_reachable",
    "preflight_target_host_state",
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
