"""Bindings from the local_ci facade to Windows SSH remote file write helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_REMOTE_FILE_WRITE_EXPORTS = ("windows_ssh_write_text",)


def windows_ssh_write_text(bindings: Mapping[str, Any], host: str, remote_path: str, content: str) -> None:
    return _binding(bindings, "_windows_probe").windows_ssh_write_text(
        host,
        remote_path,
        content,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def install_windows_remote_file_write_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_REMOTE_FILE_WRITE_EXPORTS,
) -> None:
    known_names = set(WINDOWS_REMOTE_FILE_WRITE_EXPORTS)
    write_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), write_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
