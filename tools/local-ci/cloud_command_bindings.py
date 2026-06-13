"""Compatibility composer for cloud command dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cloud_namespace_command_bindings import (
    CLOUD_NAMESPACE_COMMAND_EXPORTS,
    cmd_cloud_namespace_doctor,
    cmd_cloud_namespace_setup,
    install_cloud_namespace_command_helpers,
)
from cloud_reporting_command_bindings import (
    CLOUD_REPORTING_COMMAND_EXPORTS,
    cmd_cloud_compare,
    cmd_cloud_defaults,
    cmd_cloud_history,
    cmd_cloud_recommend,
    cmd_cloud_workflows,
    install_cloud_reporting_command_helpers,
)
from cloud_run_command_bindings import (
    CLOUD_RUN_COMMAND_EXPORTS,
    cmd_cloud_run,
    cmd_cloud_status,
    install_cloud_run_command_helpers,
)


CLOUD_COMMAND_EXPORTS = (
    *CLOUD_REPORTING_COMMAND_EXPORTS,
    *CLOUD_RUN_COMMAND_EXPORTS,
    *CLOUD_NAMESPACE_COMMAND_EXPORTS,
)


def install_cloud_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_COMMAND_EXPORTS,
) -> None:
    reporting_names = tuple(name for name in names if name in CLOUD_REPORTING_COMMAND_EXPORTS)
    run_names = tuple(name for name in names if name in CLOUD_RUN_COMMAND_EXPORTS)
    namespace_names = tuple(name for name in names if name in CLOUD_NAMESPACE_COMMAND_EXPORTS)
    known_names = set(CLOUD_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cloud_reporting_command_helpers(bindings, reporting_names)
    install_cloud_run_command_helpers(bindings, run_names)
    install_cloud_namespace_command_helpers(bindings, namespace_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
