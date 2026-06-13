"""Compatibility facade for Windows SSH/PowerShell probe dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from windows_probe_core_bindings import (
    WINDOWS_PROBE_CORE_EXPORTS,
    install_windows_probe_core_helpers,
    parse_windows_ssh_json,
    ps_literal,
    run_windows_ssh_powershell,
    windows_contract_expand_expression,
    windows_session_agent_template_path,
    windows_ssh_powershell_command,
)
from windows_remote_file_bindings import (
    WINDOWS_REMOTE_FILE_EXPORTS,
    install_windows_remote_file_helpers,
    windows_ssh_fetch_file,
    windows_ssh_read_json,
    windows_ssh_remove_path,
    windows_ssh_write_text,
)
from windows_session_probe_bindings import (
    WINDOWS_SESSION_PROBE_EXPORTS,
    bootstrap_windows_session_agent,
    install_windows_session_probe_helpers,
    probe_windows_ssh_cmake_settings,
    start_windows_session_agent_task,
)


WINDOWS_PROBE_EXPORTS = (
    *WINDOWS_PROBE_CORE_EXPORTS,
    *WINDOWS_REMOTE_FILE_EXPORTS,
    *WINDOWS_SESSION_PROBE_EXPORTS,
)


def install_windows_probe_helpers(bindings: dict[str, Any], names: tuple[str, ...] = WINDOWS_PROBE_EXPORTS) -> None:
    core_names = tuple(name for name in names if name in WINDOWS_PROBE_CORE_EXPORTS)
    remote_file_names = tuple(name for name in names if name in WINDOWS_REMOTE_FILE_EXPORTS)
    session_names = tuple(name for name in names if name in WINDOWS_SESSION_PROBE_EXPORTS)
    known_names = set(WINDOWS_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_probe_core_helpers(bindings, core_names)
    install_windows_remote_file_helpers(bindings, remote_file_names)
    install_windows_session_probe_helpers(bindings, session_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
