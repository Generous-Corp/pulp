"""Compatibility facade for target reachability dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from target_host_reachability_bindings import (
    TARGET_HOST_REACHABILITY_EXPORTS,
    ensure_host_reachable,
    install_target_host_reachability_helpers,
    preflight_target_host_state,
)
from target_ssh_reachability_bindings import (
    TARGET_SSH_REACHABILITY_EXPORTS,
    install_target_ssh_reachability_helpers,
    ssh_command_result,
    ssh_failure_detail,
    ssh_probe,
    ssh_reachable,
)
from target_utm_reachability_bindings import (
    TARGET_UTM_REACHABILITY_EXPORTS,
    install_target_utm_reachability_helpers,
    utmctl_start,
    utmctl_vm_status,
)


TARGET_REACHABILITY_EXPORTS = (
    *TARGET_SSH_REACHABILITY_EXPORTS,
    *TARGET_UTM_REACHABILITY_EXPORTS,
    *TARGET_HOST_REACHABILITY_EXPORTS,
)


def install_target_reachability_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_REACHABILITY_EXPORTS,
) -> None:
    ssh_names = tuple(name for name in names if name in TARGET_SSH_REACHABILITY_EXPORTS)
    utm_names = tuple(name for name in names if name in TARGET_UTM_REACHABILITY_EXPORTS)
    host_names = tuple(name for name in names if name in TARGET_HOST_REACHABILITY_EXPORTS)
    known_names = set(TARGET_REACHABILITY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_target_ssh_reachability_helpers(bindings, ssh_names)
    install_target_utm_reachability_helpers(bindings, utm_names)
    install_target_host_reachability_helpers(bindings, host_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
