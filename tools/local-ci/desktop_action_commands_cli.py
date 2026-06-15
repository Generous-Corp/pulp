"""Desktop automation action command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_action_command_flow import (
    emit_desktop_action_command_result,
    load_desktop_action_command_context,
    run_desktop_action_command_runner,
)
from desktop_action_dispatch import (
    desktop_click_has_target,
    desktop_click_runner,
    desktop_smoke_runner,
    windows_requires_pulp_app_selectors,
)
from desktop_inspect_commands_cli import cmd_desktop_inspect


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
    config, target, source_request, status = load_desktop_action_command_context(
        args,
        load_config_fn=load_config_fn,
        resolve_desktop_target_fn=resolve_desktop_target_fn,
        make_desktop_source_request_fn=make_desktop_source_request_fn,
        print_fn=print_fn,
    )
    if status is not None:
        return status

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

    manifest, status = run_desktop_action_command_runner(runner, print_fn=print_fn)
    if status is not None:
        return status

    return emit_desktop_action_command_result(
        action_name="smoke",
        target_name=args.target,
        manifest=manifest,
        json_output=getattr(args, "json", False),
        desktop_action_success_lines_fn=desktop_action_success_lines_fn,
        print_fn=print_fn,
    )


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
    config, target, source_request, status = load_desktop_action_command_context(
        args,
        load_config_fn=load_config_fn,
        resolve_desktop_target_fn=resolve_desktop_target_fn,
        make_desktop_source_request_fn=make_desktop_source_request_fn,
        print_fn=print_fn,
    )
    if status is not None:
        return status

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

    manifest, status = run_desktop_action_command_runner(runner, print_fn=print_fn)
    if status is not None:
        return status

    return emit_desktop_action_command_result(
        action_name="click",
        target_name=args.target,
        manifest=manifest,
        json_output=getattr(args, "json", False),
        desktop_action_success_lines_fn=desktop_action_success_lines_fn,
        print_fn=print_fn,
    )
