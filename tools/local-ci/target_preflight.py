"""Compatibility facade for target reachability and submission preflight helpers."""

from __future__ import annotations

from target_config_preflight import (
    config_material_for_targets,
    config_source_name,
    find_material_config_drift,
)
from target_host_reachability import ensure_host_reachable, preflight_target_host_state
from target_ssh_reachability import (
    ssh_command_result,
    ssh_failure_detail,
    ssh_probe,
    ssh_reachable,
)
from target_submission_build import build_submission_metadata
from target_submission_print import print_submission_metadata
from target_utm_reachability import (
    utmctl_start,
    utmctl_vm_status,
)


__all__ = (
    "build_submission_metadata",
    "config_material_for_targets",
    "config_source_name",
    "ensure_host_reachable",
    "find_material_config_drift",
    "preflight_target_host_state",
    "print_submission_metadata",
    "ssh_command_result",
    "ssh_failure_detail",
    "ssh_probe",
    "ssh_reachable",
    "utmctl_start",
    "utmctl_vm_status",
)
