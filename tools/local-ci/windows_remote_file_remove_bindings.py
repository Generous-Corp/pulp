"""Bindings from the local_ci facade to Windows SSH remote path removal helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_REMOTE_FILE_REMOVE_EXPORTS = ("windows_ssh_remove_path",)


def windows_ssh_remove_path(bindings: Mapping[str, Any], host: str, remote_path: str) -> None:
    return _binding(bindings, "_windows_probe").windows_ssh_remove_path(
        host,
        remote_path,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
    )


def install_windows_remote_file_remove_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_REMOTE_FILE_REMOVE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
