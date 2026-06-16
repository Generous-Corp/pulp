"""Compatibility facade for target reachability helpers."""

from __future__ import annotations

from target_host_reachability import ensure_host_reachable, preflight_target_host_state
from target_ssh_reachability import ssh_command_result, ssh_failure_detail, ssh_probe, ssh_reachable
from target_utm_reachability import utmctl_start, utmctl_vm_status


__all__ = (
    "ensure_host_reachable",
    "preflight_target_host_state",
    "ssh_command_result",
    "ssh_failure_detail",
    "ssh_probe",
    "ssh_reachable",
    "utmctl_start",
    "utmctl_vm_status",
)
