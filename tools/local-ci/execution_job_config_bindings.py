"""Bindings from the local_ci facade to validation job config helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


EXECUTION_JOB_CONFIG_EXPORTS = (
    "config_for_job_execution",
    "submission_target_state",
    "resolve_ssh_target_execution",
)


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


def install_execution_job_config_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_JOB_CONFIG_EXPORTS,
) -> None:
    known_names = set(EXECUTION_JOB_CONFIG_EXPORTS)
    config_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), config_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
