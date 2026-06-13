"""Compatibility composer for PR-oriented local-CI command bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from local_ci_pr_check_command_bindings import (
    LOCAL_CI_PR_CHECK_COMMAND_EXPORTS,
    cmd_check,
    install_local_ci_pr_check_command_helpers,
)
from local_ci_pr_list_command_bindings import (
    LOCAL_CI_PR_LIST_COMMAND_EXPORTS,
    cmd_list,
    install_local_ci_pr_list_command_helpers,
)
from local_ci_pr_ship_command_bindings import (
    LOCAL_CI_PR_SHIP_COMMAND_EXPORTS,
    cmd_ship,
    install_local_ci_pr_ship_command_helpers,
)


LOCAL_CI_PR_COMMAND_EXPORTS = (
    *LOCAL_CI_PR_SHIP_COMMAND_EXPORTS,
    *LOCAL_CI_PR_CHECK_COMMAND_EXPORTS,
    *LOCAL_CI_PR_LIST_COMMAND_EXPORTS,
)


def install_local_ci_pr_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOCAL_CI_PR_COMMAND_EXPORTS,
) -> None:
    ship_names = tuple(name for name in names if name in LOCAL_CI_PR_SHIP_COMMAND_EXPORTS)
    check_names = tuple(name for name in names if name in LOCAL_CI_PR_CHECK_COMMAND_EXPORTS)
    list_names = tuple(name for name in names if name in LOCAL_CI_PR_LIST_COMMAND_EXPORTS)
    known_names = set(LOCAL_CI_PR_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_ci_pr_ship_command_helpers(bindings, ship_names)
    install_local_ci_pr_check_command_helpers(bindings, check_names)
    install_local_ci_pr_list_command_helpers(bindings, list_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
