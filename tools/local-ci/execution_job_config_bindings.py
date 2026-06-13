"""Bindings from the local_ci facade to validation job config helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import print_binding as _print_binding


def config_for_job_execution(bindings: Mapping[str, Any], job: dict, config: dict) -> dict:
    return _binding(bindings, "_execution").config_for_job_execution(
        job,
        config,
        load_config_file_fn=_binding(bindings, "load_config_file"),
        warn_fn=_print_binding(bindings),
    )


def submission_target_state(bindings: Mapping[str, Any], job: dict, target_name: str) -> dict:
    return _binding(bindings, "_execution").submission_target_state(job, target_name)


def resolve_ssh_target_execution(
    bindings: Mapping[str, Any],
    job: dict,
    target_name: str,
    target_cfg: dict,
    defaults: dict,
) -> tuple[str | None, str | None]:
    return _binding(bindings, "_execution").resolve_ssh_target_execution(
        job,
        target_name,
        target_cfg,
        defaults,
        ensure_host_reachable_fn=_binding(bindings, "ensure_host_reachable"),
    )
