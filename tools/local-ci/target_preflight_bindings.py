"""Compatibility installer for local_ci target preflight facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from target_config_preflight_bindings import (
    TARGET_CONFIG_PREFLIGHT_EXPORTS,
    config_material_for_targets,
    config_source_name,
    find_material_config_drift,
    install_target_config_preflight_helpers,
)
from target_reachability_bindings import (
    TARGET_REACHABILITY_EXPORTS,
    ensure_host_reachable,
    install_target_reachability_helpers,
    preflight_target_host_state,
    ssh_command_result,
    ssh_failure_detail,
    ssh_probe,
    ssh_reachable,
    utmctl_start,
    utmctl_vm_status,
)
from target_submission_bindings import (
    TARGET_SUBMISSION_EXPORTS,
    build_submission_metadata,
    install_target_submission_helpers,
    print_submission_metadata,
)


TARGET_PREFLIGHT_EXPORTS = (
    *TARGET_REACHABILITY_EXPORTS,
    *TARGET_CONFIG_PREFLIGHT_EXPORTS,
    *TARGET_SUBMISSION_EXPORTS,
)


def install_target_preflight_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_PREFLIGHT_EXPORTS,
) -> None:
    reachability_names = tuple(name for name in names if name in TARGET_REACHABILITY_EXPORTS)
    config_names = tuple(name for name in names if name in TARGET_CONFIG_PREFLIGHT_EXPORTS)
    submission_names = tuple(name for name in names if name in TARGET_SUBMISSION_EXPORTS)
    known_names = set(TARGET_PREFLIGHT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_target_reachability_helpers(bindings, reachability_names)
    install_target_config_preflight_helpers(bindings, config_names)
    install_target_submission_helpers(bindings, submission_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
