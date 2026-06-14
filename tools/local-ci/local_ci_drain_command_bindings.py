"""Facade bindings for the local-CI drain command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


LOCAL_CI_DRAIN_COMMAND_EXPORTS = (
    "cmd_drain",
)


def cmd_drain(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_drain(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        drain_pending_jobs_fn=_binding(bindings, "drain_pending_jobs"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        drain_runner_active_line_fn=_binding(bindings, "drain_runner_active_line"),
        notify_fn=_binding(bindings, "notify"),
    )
