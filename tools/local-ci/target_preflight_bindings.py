"""Compatibility installer for local_ci target preflight facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from target_config_preflight_bindings import (
    config_material_for_targets,
    config_source_name,
    find_material_config_drift,
)
from target_reachability_bindings import (
    ensure_host_reachable,
    preflight_target_host_state,
    ssh_command_result,
    ssh_failure_detail,
    ssh_probe,
    ssh_reachable,
    utmctl_start,
    utmctl_vm_status,
)
from target_submission_bindings import (
    build_submission_metadata,
    print_submission_metadata,
)


TARGET_PREFLIGHT_EXPORTS = (
    "ssh_probe",
    "ssh_reachable",
    "ssh_failure_detail",
    "ssh_command_result",
    "utmctl_vm_status",
    "utmctl_start",
    "ensure_host_reachable",
    "config_source_name",
    "config_material_for_targets",
    "find_material_config_drift",
    "preflight_target_host_state",
    "build_submission_metadata",
    "print_submission_metadata",
)


def install_target_preflight_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_PREFLIGHT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
