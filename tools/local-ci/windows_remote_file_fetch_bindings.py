"""Bindings from the local_ci facade to Windows SSH remote file fetch helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_REMOTE_FILE_FETCH_EXPORTS = ("windows_ssh_fetch_file",)


def windows_ssh_fetch_file(
    bindings: Mapping[str, Any],
    host: str,
    remote_path: str,
    local_path,
    *,
    optional: bool = False,
    timeout: int = 60,
) -> bool:
    return _binding(bindings, "_windows_probe").windows_ssh_fetch_file(
        host,
        remote_path,
        local_path,
        optional=optional,
        timeout=timeout,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
    )


def install_windows_remote_file_fetch_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_REMOTE_FILE_FETCH_EXPORTS,
) -> None:
    known_names = set(WINDOWS_REMOTE_FILE_FETCH_EXPORTS)
    fetch_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), fetch_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
