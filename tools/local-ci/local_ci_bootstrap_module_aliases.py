"""Private module alias installation for the local_ci.py facade."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


BOOTSTRAP_MODULE_ALIASES = {
    "_state_paths": "_state_paths",
    "_footprint": "_footprint",
    "_cleanup": "_cleanup",
    "_cleanup_cli": "_cleanup_cli",
    "_cli_dispatch": "_cli_dispatch",
    "_cli_parser": "_cli_parser",
    "_cloud": "_cloud",
    "_desktop_action_commands_cli": "_desktop_action_commands_cli",
    "_desktop_actions": "_desktop_actions",
    "_desktop_artifacts": "_desktop_artifacts",
    "_desktop_commands_cli": "_desktop_commands_cli",
    "_desktop_cli": "_desktop_cli",
    "_desktop_doctor": "_desktop_doctor",
    "_desktop_setup_commands_cli": "_desktop_setup_commands_cli",
    "_evidence_cli": "_evidence_cli",
    "_execution": "_execution",
    "_git_helpers": "_git_helpers",
    "_github_workflows": "_github_workflows",
    "_io_utils": "_io_utils",
    "_job_queue": "_job_queue",
    "_linux_desktop_action": "_linux_desktop_action",
    "_linux_target": "_linux_target",
    "_local_ci_commands_cli": "_local_ci_commands_cli",
    "_logs_cli": "_logs_cli",
    "_macos_desktop": "_macos_desktop",
    "_macos_desktop_action": "_macos_desktop_action",
    "_notifications": "_notifications",
    "_normalize": "_normalize",
    "_provenance": "_provenance",
    "_queue_commands_cli": "_queue_commands_cli",
    "_queue_lifecycle": "_queue_lifecycle",
    "_queue_orchestrator": "_queue_orchestrator",
    "_reporting": "_reporting",
    "_runner_state": "_runner_state",
    "_source_prep": "_source_prep",
    "_ssh_bundle": "_ssh_bundle",
    "_ssh_subprocess": "_ssh_subprocess",
    "_target_preflight": "_target_preflight",
    "_targets": "_targets",
    "_windows_desktop_action": "_windows_desktop_action",
    "_windows_probe": "_windows_probe",
    "_windows_target": "_windows_target",
    "evidence_index_module": "_evidence_index",
    "LockBusyError": "LockBusyError",
}


def install_bootstrap_module_aliases(
    bindings: dict[str, Any],
    module_globals: Mapping[str, Any],
) -> None:
    bindings.update(
        {
            binding_name: module_globals[module_name]
            for binding_name, module_name in BOOTSTRAP_MODULE_ALIASES.items()
        }
    )
