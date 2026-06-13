"""Compatibility facade for top-level local-CI command dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from local_ci_pr_command_bindings import (
    LOCAL_CI_PR_COMMAND_EXPORTS,
    cmd_check,
    cmd_list,
    cmd_ship,
    install_local_ci_pr_command_helpers,
)
from local_ci_queue_command_bindings import (
    LOCAL_CI_QUEUE_COMMAND_EXPORTS,
    cmd_drain,
    cmd_enqueue,
    cmd_run,
)
from local_ci_status_command_bindings import (
    LOCAL_CI_STATUS_COMMAND_EXPORTS,
    cmd_status,
)
from local_ci_submission_bindings import (
    LOCAL_CI_SUBMISSION_EXPORTS,
    resolve_submission_options,
)


LOCAL_CI_COMMAND_EXPORTS = (
    *LOCAL_CI_SUBMISSION_EXPORTS,
    *LOCAL_CI_QUEUE_COMMAND_EXPORTS,
    *LOCAL_CI_PR_COMMAND_EXPORTS,
    *LOCAL_CI_STATUS_COMMAND_EXPORTS,
)


def install_local_ci_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOCAL_CI_COMMAND_EXPORTS,
) -> None:
    pr_names = tuple(name for name in names if name in LOCAL_CI_PR_COMMAND_EXPORTS)
    local_export_names = (
        LOCAL_CI_SUBMISSION_EXPORTS
        + LOCAL_CI_QUEUE_COMMAND_EXPORTS
        + LOCAL_CI_STATUS_COMMAND_EXPORTS
    )
    local_names = tuple(name for name in names if name in local_export_names)
    known_names = set(LOCAL_CI_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_ci_pr_command_helpers(bindings, pr_names)
    install_local_helpers(bindings, globals(), local_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
