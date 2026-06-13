"""Compatibility composer for Windows SSH remote file transfer bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from windows_remote_file_fetch_bindings import (
    WINDOWS_REMOTE_FILE_FETCH_EXPORTS,
    install_windows_remote_file_fetch_helpers,
    windows_ssh_fetch_file,
)
from windows_remote_file_read_bindings import (
    WINDOWS_REMOTE_FILE_READ_EXPORTS,
    install_windows_remote_file_read_helpers,
    windows_ssh_read_json,
)
from windows_remote_file_remove_bindings import (
    WINDOWS_REMOTE_FILE_REMOVE_EXPORTS,
    install_windows_remote_file_remove_helpers,
    windows_ssh_remove_path,
)


WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS = (
    *WINDOWS_REMOTE_FILE_FETCH_EXPORTS,
    *WINDOWS_REMOTE_FILE_READ_EXPORTS,
    *WINDOWS_REMOTE_FILE_REMOVE_EXPORTS,
)


def install_windows_remote_file_transfer_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS,
) -> None:
    fetch_names = tuple(name for name in names if name in WINDOWS_REMOTE_FILE_FETCH_EXPORTS)
    read_names = tuple(name for name in names if name in WINDOWS_REMOTE_FILE_READ_EXPORTS)
    remove_names = tuple(name for name in names if name in WINDOWS_REMOTE_FILE_REMOVE_EXPORTS)
    known_names = set(WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_remote_file_fetch_helpers(bindings, fetch_names)
    install_windows_remote_file_read_helpers(bindings, read_names)
    install_windows_remote_file_remove_helpers(bindings, remove_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
