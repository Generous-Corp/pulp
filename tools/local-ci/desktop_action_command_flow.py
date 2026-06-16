"""Shared desktop action command orchestration helpers."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json


def load_desktop_action_command_context(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    make_desktop_source_request_fn: Callable[[argparse.Namespace], dict],
    print_fn: Callable[[str], None],
) -> tuple[dict | None, dict | None, dict | None, int | None]:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
        source_request = make_desktop_source_request_fn(args)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return None, None, None, 1
    return config, target, source_request, None


def run_desktop_action_command_runner(
    runner: Callable[[], dict],
    *,
    print_fn: Callable[[str], None],
) -> tuple[dict | None, int | None]:
    try:
        return runner(), None
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return None, 1


def print_desktop_action_lines(lines: list[str], *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def emit_desktop_action_command_result(
    *,
    action_name: str,
    target_name: str,
    manifest: dict,
    json_output: bool,
    desktop_action_success_lines_fn: Callable[[str, str, dict], list[str]],
    print_fn: Callable[[str], None],
) -> int:
    if json_output:
        print_fn(json.dumps(manifest, indent=2))
        return 0

    print_desktop_action_lines(
        desktop_action_success_lines_fn(action_name, target_name, manifest),
        print_fn=print_fn,
    )
    return 0
