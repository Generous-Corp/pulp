"""Compatibility installer for local_ci cloud/GitHub facade bindings."""

from __future__ import annotations

from typing import Any

from cloud_command_bindings import (
    CLOUD_COMMAND_EXPORTS,
    cmd_cloud_compare,
    cmd_cloud_defaults,
    cmd_cloud_history,
    cmd_cloud_namespace_doctor,
    cmd_cloud_namespace_setup,
    cmd_cloud_recommend,
    cmd_cloud_run,
    cmd_cloud_status,
    cmd_cloud_workflows,
    install_cloud_command_helpers,
)
from cloud_github_bindings import (
    CLOUD_GITHUB_EXPORTS,
    gh_available,
    gh_pr_comment,
    gh_pr_create,
    gh_pr_head,
    gh_pr_list_open,
    gh_pr_merge,
    gh_run_view,
    gh_workflow_dispatch,
    install_cloud_github_helpers,
)
from cloud_module_attr_bindings import (
    CLOUD_MODULE_ATTR_EXPORTS,
    install_cloud_module_attr_helpers,
)
from cloud_record_bindings import (
    CLOUD_RECORD_EXPORTS,
    cloud_record_summary,
    format_ci_comment,
    install_cloud_record_helpers,
    list_cloud_records,
    open_pr_list_lines,
)


CLOUD_HELPER_EXPORTS = (
    *CLOUD_MODULE_ATTR_EXPORTS,
    *CLOUD_COMMAND_EXPORTS,
    *CLOUD_GITHUB_EXPORTS,
    *CLOUD_RECORD_EXPORTS,
)


def install_cloud_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLOUD_HELPER_EXPORTS) -> None:
    module_attr_names = tuple(name for name in names if name in CLOUD_MODULE_ATTR_EXPORTS)
    command_names = tuple(name for name in names if name in CLOUD_COMMAND_EXPORTS)
    github_names = tuple(name for name in names if name in CLOUD_GITHUB_EXPORTS)
    record_names = tuple(name for name in names if name in CLOUD_RECORD_EXPORTS)
    known_names = set(CLOUD_HELPER_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cloud_module_attr_helpers(bindings, module_attr_names)
    install_cloud_command_helpers(bindings, command_names)
    install_cloud_github_helpers(bindings, github_names)
    install_cloud_record_helpers(bindings, record_names)
    if unknown_names:
        install_cloud_module_attr_helpers(bindings, unknown_names)
