"""Facade bindings for the local-CI run command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


LOCAL_CI_RUN_COMMAND_EXPORTS = (
    "cmd_run",
)


def cmd_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_run(
        args,
        resolve_submission_options_fn=_binding(bindings, "resolve_submission_options"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        gh_workflow_dispatch_fn=_binding(bindings, "gh_workflow_dispatch"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        enqueue_command_result_line_fn=_binding(bindings, "enqueue_command_result_line"),
        wait_for_job_fn=_binding(bindings, "wait_for_job"),
        load_job_fn=_binding(bindings, "load_job"),
        print_result_fn=_binding(bindings, "print_result"),
        notify_fn=_binding(bindings, "notify"),
    )
