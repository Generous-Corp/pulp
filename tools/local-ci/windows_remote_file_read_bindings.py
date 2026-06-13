"""Bindings from the local_ci facade to Windows SSH remote JSON read helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_REMOTE_FILE_READ_EXPORTS = ("windows_ssh_read_json",)


def windows_ssh_read_json(
    bindings: Mapping[str, Any],
    host: str,
    remote_path: str,
    *,
    timeout: int = 30,
    optional: bool = False,
) -> dict | None:
    return _binding(bindings, "_windows_probe").windows_ssh_read_json(
        host,
        remote_path,
        timeout=timeout,
        optional=optional,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
    )


def install_windows_remote_file_read_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_REMOTE_FILE_READ_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
