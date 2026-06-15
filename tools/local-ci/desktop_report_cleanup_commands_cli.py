"""Desktop automation report-cleanup command orchestration."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path
import shutil

from desktop_command_flow import emit_desktop_command_result, load_desktop_command_config


def cmd_desktop_cleanup(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    prune_desktop_run_manifests_fn: Callable[..., list[Path]],
    write_desktop_run_rollups_fn: Callable[..., None],
    desktop_cleanup_empty_line_fn: Callable[[], str],
    desktop_cleanup_lines_fn: Callable[[list[Path]], list[str]],
    remove_tree_fn: Callable[..., None] = shutil.rmtree,
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    older_than = args.older_than_days if args.older_than_days is not None else config["desktop_automation"]["retention_days"]
    paths = prune_desktop_run_manifests_fn(
        config,
        target_name=args.target,
        older_than_days=older_than,
        keep_last=args.keep_last,
    )
    if not paths:
        print_fn(desktop_cleanup_empty_line_fn())
        return 0

    for path in paths:
        remove_tree_fn(path, ignore_errors=False)

    write_desktop_run_rollups_fn(config, target_name=args.target if args.target else None)
    if args.target is not None:
        write_desktop_run_rollups_fn(config)

    return emit_desktop_command_result(
        payload={"removed": [str(path) for path in paths]},
        json_output=getattr(args, "json", False),
        text_lines=desktop_cleanup_lines_fn(paths),
        print_fn=print_fn,
    )


__all__ = ["cmd_desktop_cleanup"]
