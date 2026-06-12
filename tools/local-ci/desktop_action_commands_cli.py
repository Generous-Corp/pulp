"""Desktop automation action command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json

from desktop_action_dispatch import (
    desktop_click_has_target,
    desktop_click_runner,
    desktop_smoke_runner,
    windows_requires_pulp_app_selectors,
)
from desktop_inspect_commands_cli import cmd_desktop_inspect


def _print_lines(lines, *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def cmd_desktop_smoke(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    make_desktop_source_request_fn: Callable[[argparse.Namespace], dict],
    run_macos_local_smoke_fn: Callable[..., dict],
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
    run_windows_session_agent_action_fn: Callable[..., dict],
    desktop_action_success_lines_fn: Callable[[str, str, dict], list[str]],
    sys_platform: str,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
        source_request = make_desktop_source_request_fn(args)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    runner, error = desktop_smoke_runner(
        args=args,
        config=config,
        target=target,
        source_request=source_request,
        run_macos_local_smoke_fn=run_macos_local_smoke_fn,
        run_linux_xvfb_remote_action_fn=run_linux_xvfb_remote_action_fn,
        run_windows_session_agent_action_fn=run_windows_session_agent_action_fn,
        sys_platform=sys_platform,
    )
    if error:
        print_fn(f"Error: {error}")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(manifest, indent=2))
        return 0

    _print_lines(desktop_action_success_lines_fn("smoke", args.target, manifest), print_fn=print_fn)
    return 0


def cmd_desktop_click(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    make_desktop_source_request_fn: Callable[[argparse.Namespace], dict],
    run_macos_local_smoke_fn: Callable[..., dict],
    run_linux_xvfb_remote_action_fn: Callable[..., dict],
    run_windows_session_agent_action_fn: Callable[..., dict],
    desktop_action_success_lines_fn: Callable[[str, str, dict], list[str]],
    sys_platform: str,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
        source_request = make_desktop_source_request_fn(args)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    runner, error = desktop_click_runner(
        args=args,
        config=config,
        target=target,
        source_request=source_request,
        run_macos_local_smoke_fn=run_macos_local_smoke_fn,
        run_linux_xvfb_remote_action_fn=run_linux_xvfb_remote_action_fn,
        run_windows_session_agent_action_fn=run_windows_session_agent_action_fn,
        sys_platform=sys_platform,
    )
    if error:
        print_fn(f"Error: {error}")
        return 1
    if not desktop_click_has_target(args):
        print_fn("Error: desktop click requires --click or one view-target selector.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(manifest, indent=2))
        return 0

    _print_lines(desktop_action_success_lines_fn("click", args.target, manifest), print_fn=print_fn)
    return 0
