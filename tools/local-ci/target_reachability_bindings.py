"""Compatibility facade for target reachability dependency bindings."""

from __future__ import annotations

from target_host_reachability_bindings import (
    TARGET_HOST_REACHABILITY_EXPORTS,
    ensure_host_reachable,
    preflight_target_host_state,
)
from target_ssh_reachability_bindings import (
    TARGET_SSH_REACHABILITY_EXPORTS,
    ssh_command_result,
    ssh_failure_detail,
    ssh_probe,
    ssh_reachable,
)
from target_utm_reachability_bindings import (
    TARGET_UTM_REACHABILITY_EXPORTS,
    utmctl_start,
    utmctl_vm_status,
)


TARGET_REACHABILITY_EXPORTS = (
    *TARGET_SSH_REACHABILITY_EXPORTS,
    *TARGET_UTM_REACHABILITY_EXPORTS,
    *TARGET_HOST_REACHABILITY_EXPORTS,
)
