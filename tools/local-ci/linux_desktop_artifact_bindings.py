"""Facade bindings for Linux desktop SSH artifact helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def fetch_ssh_artifact(
    bindings: Mapping[str, Any],
    host: str,
    remote_path: str,
    local_path,
    *,
    optional: bool = False,
    timeout: int = 60,
) -> bool:
    return _binding(bindings, "_linux_desktop_action").fetch_ssh_artifact(
        host,
        remote_path,
        local_path,
        optional=optional,
        timeout=timeout,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def cleanup_remote_ssh_dir(bindings: Mapping[str, Any], host: str, remote_dir_expr: str) -> None:
    return _binding(bindings, "_linux_desktop_action").cleanup_remote_ssh_dir(
        host,
        remote_dir_expr,
        ssh_command_result_fn=_binding(bindings, "ssh_command_result"),
    )
