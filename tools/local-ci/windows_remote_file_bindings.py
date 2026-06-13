"""Compatibility facade for Windows SSH remote file dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from windows_remote_file_transfer_bindings import (
    WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS,
    install_windows_remote_file_transfer_helpers,
    windows_ssh_fetch_file,
    windows_ssh_read_json,
    windows_ssh_remove_path,
)
from windows_remote_file_write_bindings import (
    WINDOWS_REMOTE_FILE_WRITE_EXPORTS,
    install_windows_remote_file_write_helpers,
    windows_ssh_write_text,
)


WINDOWS_REMOTE_FILE_EXPORTS = (
    *WINDOWS_REMOTE_FILE_WRITE_EXPORTS,
    *WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS,
)


def install_windows_remote_file_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_REMOTE_FILE_EXPORTS,
) -> None:
    write_names = tuple(name for name in names if name in WINDOWS_REMOTE_FILE_WRITE_EXPORTS)
    transfer_names = tuple(name for name in names if name in WINDOWS_REMOTE_FILE_TRANSFER_EXPORTS)
    known_names = set(WINDOWS_REMOTE_FILE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_remote_file_write_helpers(bindings, write_names)
    install_windows_remote_file_transfer_helpers(bindings, transfer_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
