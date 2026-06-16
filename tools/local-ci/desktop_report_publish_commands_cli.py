"""Desktop automation publish-report command orchestration."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_command_config,
    require_desktop_run_manifests,
    run_desktop_command_step,
)


def cmd_desktop_publish(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    stage_desktop_publish_report_fn: Callable[..., dict],
    desktop_publish_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not require_desktop_run_manifests(
        manifests,
        empty_line="No desktop automation runs found.",
        print_fn=print_fn,
    ):
        return 0

    manifests = manifests[: args.limit]
    output_dir = Path(args.output).expanduser() if args.output else None

    report, status = run_desktop_command_step(
        lambda: stage_desktop_publish_report_fn(config, manifests, output_dir=output_dir, label=args.label),
        print_fn=print_fn,
    )
    if status is not None:
        return status

    return emit_desktop_command_result(
        payload=report,
        json_output=getattr(args, "json", False),
        text_lines=desktop_publish_lines_fn(report),
        print_fn=print_fn,
    )


__all__ = ["cmd_desktop_publish"]
