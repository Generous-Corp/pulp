"""Shared desktop management command flow helpers."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any
import json


def load_desktop_command_config(
    *,
    load_config_fn: Callable[[], dict],
    print_fn: Callable[[str], None],
) -> tuple[dict | None, int | None]:
    try:
        return load_config_fn(), None
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return None, 1


def run_desktop_command_step(
    step: Callable[[], Any],
    *,
    print_fn: Callable[[str], None],
    error_prefix: str = "Error: ",
) -> tuple[Any | None, int | None]:
    try:
        return step(), None
    except Exception as exc:
        print_fn(f"{error_prefix}{exc}")
        return None, 1


def print_desktop_command_lines(lines: list[str], *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def emit_desktop_command_result(
    *,
    payload: dict,
    json_output: bool,
    text_lines: list[str],
    print_fn: Callable[[str], None],
) -> int:
    if json_output:
        print_fn(json.dumps(payload, indent=2))
        return 0

    print_desktop_command_lines(text_lines, print_fn=print_fn)
    return 0


def require_desktop_run_manifests(
    manifests: list[dict],
    *,
    empty_line: str,
    print_fn: Callable[[str], None],
) -> bool:
    if manifests:
        return True
    print_fn(empty_line)
    return False
