"""Desktop automation recent-report command orchestration."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_command_config,
    require_desktop_run_manifests,
)


def cmd_desktop_recent(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_recent_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
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

    run_summaries = [desktop_run_summary_fn(config, manifest) for manifest in manifests]
    return emit_desktop_command_result(
        payload={"runs": manifests},
        json_output=getattr(args, "json", False),
        text_lines=desktop_recent_lines_fn(run_summaries, short_sha_fn=short_sha_fn),
        print_fn=print_fn,
    )


__all__ = ["cmd_desktop_recent"]
