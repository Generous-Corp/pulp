"""Cloud workflow listing command orchestration."""
from __future__ import annotations

from collections.abc import Callable
import argparse


def cmd_cloud_workflows(
    _args: argparse.Namespace,
    *,
    builtin_github_workflows: dict,
    cloud_workflow_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    for line in cloud_workflow_lines_fn(builtin_github_workflows):
        print_fn(line)
    return 0


__all__ = ["cmd_cloud_workflows"]
