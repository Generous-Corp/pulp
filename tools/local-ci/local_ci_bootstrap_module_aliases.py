"""Private module alias installation for the local_ci.py facade."""

from __future__ import annotations

from typing import Any

import cleanup as _cleanup
import cleanup_cli as _cleanup_cli
import cli_dispatch as _cli_dispatch
import cli_parser as _cli_parser
import cloud as _cloud
import desktop_action_commands_cli as _desktop_action_commands_cli
import desktop_actions as _desktop_actions
import desktop_artifacts as _desktop_artifacts
import desktop_commands_cli as _desktop_commands_cli
import desktop_cli as _desktop_cli
import desktop_doctor as _desktop_doctor
import desktop_setup_commands_cli as _desktop_setup_commands_cli
import evidence_cli as _evidence_cli
import evidence_index as _evidence_index
import execution as _execution
import footprint as _footprint
import git_helpers as _git_helpers
import github_workflows as _github_workflows
import io_utils as _io_utils
from io_utils import LockBusyError
import job_queue as _job_queue
import linux_desktop_action as _linux_desktop_action
import linux_target as _linux_target
import local_ci_commands_cli as _local_ci_commands_cli
import logs_cli as _logs_cli
import io_utils_design_parity as _io_utils_design_parity
import macos_desktop as _macos_desktop
import macos_desktop_action as _macos_desktop_action
import macos_desktop_video as _macos_desktop_video
import macos_terminal_proof as _macos_terminal_proof
import macos_terminal_runner as _macos_terminal_runner
import macos_window_video_select as _macos_window_video_select
import notifications as _notifications
import normalize as _normalize
import reporting_review as _reporting_review
import reporting_video as _reporting_video
import video_artifacts as _video_artifacts
import provenance as _provenance
import queue_commands_cli as _queue_commands_cli
import queue_lifecycle as _queue_lifecycle
import queue_orchestrator as _queue_orchestrator
import reporting as _reporting
import runner_state as _runner_state
import source_prep as _source_prep
import ssh_bundle as _ssh_bundle
import ssh_subprocess as _ssh_subprocess
import state_paths as _state_paths
import target_preflight as _target_preflight
import targets as _targets
import windows_desktop_action as _windows_desktop_action
import windows_probe as _windows_probe
import windows_target as _windows_target


BOOTSTRAP_MODULE_ALIASES = {
    "_state_paths": _state_paths,
    "_footprint": _footprint,
    "_cleanup": _cleanup,
    "_cleanup_cli": _cleanup_cli,
    "_cli_dispatch": _cli_dispatch,
    "_cli_parser": _cli_parser,
    "_cloud": _cloud,
    "_desktop_action_commands_cli": _desktop_action_commands_cli,
    "_desktop_actions": _desktop_actions,
    "_desktop_artifacts": _desktop_artifacts,
    "_desktop_commands_cli": _desktop_commands_cli,
    "_desktop_cli": _desktop_cli,
    "_desktop_doctor": _desktop_doctor,
    "_desktop_setup_commands_cli": _desktop_setup_commands_cli,
    "_evidence_cli": _evidence_cli,
    "_execution": _execution,
    "_git_helpers": _git_helpers,
    "_github_workflows": _github_workflows,
    "_io_utils": _io_utils,
    "_job_queue": _job_queue,
    "_linux_desktop_action": _linux_desktop_action,
    "_linux_target": _linux_target,
    "_local_ci_commands_cli": _local_ci_commands_cli,
    "_logs_cli": _logs_cli,
    "_io_utils_design_parity": _io_utils_design_parity,
    "_macos_desktop": _macos_desktop,
    "_macos_desktop_action": _macos_desktop_action,
    "_macos_desktop_video": _macos_desktop_video,
    "_macos_terminal_proof": _macos_terminal_proof,
    "_macos_terminal_runner": _macos_terminal_runner,
    "_macos_window_video_select": _macos_window_video_select,
    "_notifications": _notifications,
    "_reporting_review": _reporting_review,
    "_reporting_video": _reporting_video,
    "_video_artifacts": _video_artifacts,
    "_normalize": _normalize,
    "_provenance": _provenance,
    "_queue_commands_cli": _queue_commands_cli,
    "_queue_lifecycle": _queue_lifecycle,
    "_queue_orchestrator": _queue_orchestrator,
    "_reporting": _reporting,
    "_runner_state": _runner_state,
    "_source_prep": _source_prep,
    "_ssh_bundle": _ssh_bundle,
    "_ssh_subprocess": _ssh_subprocess,
    "_target_preflight": _target_preflight,
    "_targets": _targets,
    "_windows_desktop_action": _windows_desktop_action,
    "_windows_probe": _windows_probe,
    "_windows_target": _windows_target,
    "evidence_index_module": _evidence_index,
    "LockBusyError": LockBusyError,
}


def install_bootstrap_module_aliases(bindings: dict[str, Any]) -> None:
    bindings.update(BOOTSTRAP_MODULE_ALIASES)
