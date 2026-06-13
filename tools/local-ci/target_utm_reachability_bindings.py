"""Bindings from the local_ci facade to UTM target reachability helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


TARGET_UTM_REACHABILITY_EXPORTS = (
    "utmctl_vm_status",
    "utmctl_start",
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
