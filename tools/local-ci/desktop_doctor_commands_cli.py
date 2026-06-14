"""Desktop automation doctor command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_target_command_context,
)
from desktop_setup_command_format import desktop_doctor_lines, desktop_doctor_payload


def cmd_desktop_doctor(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, target, status = load_desktop_target_command_context(
        args.target,
        load_config_fn=load_config_fn,
        resolve_desktop_target_fn=resolve_desktop_target_fn,
        print_fn=print_fn,
    )
    if status is not None:
        return status

    checks = desktop_doctor_checks_fn(config, args.target)
    all_ok = True
    for check in checks:
        if check.get("required", True):
            all_ok = all_ok and check["ok"]
    emit_desktop_command_result(
        payload=desktop_doctor_payload(args, target=target, checks=checks, all_ok=all_ok),
        json_output=getattr(args, "json", False),
        text_lines=desktop_doctor_lines(args, target=target, checks=checks),
        print_fn=print_fn,
    )
    return 0 if all_ok else 1


__all__ = ["cmd_desktop_doctor"]
