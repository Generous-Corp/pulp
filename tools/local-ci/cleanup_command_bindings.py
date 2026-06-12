"""Bindings from the local_ci facade to cleanup command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def print_local_ci_state_footprint(bindings: Mapping[str, Any], *, indent: str = "") -> None:
    return _binding(bindings, "_cleanup_cli").print_local_ci_state_footprint(
        local_ci_state_footprint_fn=_binding(bindings, "local_ci_state_footprint"),
        state_footprint_lines_fn=_binding(bindings, "state_footprint_lines"),
        indent=indent,
    )


def print_local_ci_cleanup_plan(bindings: Mapping[str, Any], plan: dict, *, dry_run: bool) -> None:
    return _binding(bindings, "_cleanup_cli").print_local_ci_cleanup_plan(
        plan,
        dry_run=dry_run,
        cleanup_plan_lines_fn=_binding(bindings, "cleanup_plan_lines"),
    )


def cmd_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cleanup_cli").cmd_cleanup(
        args,
        load_queue_fn=_binding(bindings, "load_queue"),
        collect_cleanup_plan_fn=_binding(bindings, "collect_local_ci_cleanup_plan"),
        apply_cleanup_plan_fn=_binding(bindings, "apply_local_ci_cleanup_plan"),
        print_cleanup_plan_fn=_binding(bindings, "print_local_ci_cleanup_plan"),
        print_state_footprint_fn=_binding(bindings, "print_local_ci_state_footprint"),
        format_size_fn=_binding(bindings, "format_size_bytes"),
        describe_path_fn=_binding(bindings, "describe_path_for_cleanup"),
    )
