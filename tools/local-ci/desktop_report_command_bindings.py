"""Compatibility facade for desktop report command bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_report_cleanup_command_bindings import (
    DESKTOP_REPORT_CLEANUP_COMMAND_EXPORTS,
    cmd_desktop_cleanup,
    install_desktop_report_cleanup_command_helpers,
)
from desktop_report_proof_command_bindings import (
    DESKTOP_REPORT_PROOF_COMMAND_EXPORTS,
    cmd_desktop_proof,
    install_desktop_report_proof_command_helpers,
)
from desktop_report_publish_command_bindings import (
    DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS,
    cmd_desktop_publish,
    install_desktop_report_publish_command_helpers,
)
from desktop_report_recent_command_bindings import (
    DESKTOP_REPORT_RECENT_COMMAND_EXPORTS,
    cmd_desktop_recent,
    install_desktop_report_recent_command_helpers,
)


DESKTOP_REPORT_COMMAND_EXPORTS = (
    *DESKTOP_REPORT_RECENT_COMMAND_EXPORTS,
    *DESKTOP_REPORT_PROOF_COMMAND_EXPORTS,
    *DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS,
    *DESKTOP_REPORT_CLEANUP_COMMAND_EXPORTS,
)


def install_desktop_report_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_REPORT_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_REPORT_COMMAND_EXPORTS)
    recent_names = tuple(name for name in names if name in DESKTOP_REPORT_RECENT_COMMAND_EXPORTS)
    proof_names = tuple(name for name in names if name in DESKTOP_REPORT_PROOF_COMMAND_EXPORTS)
    publish_names = tuple(name for name in names if name in DESKTOP_REPORT_PUBLISH_COMMAND_EXPORTS)
    cleanup_names = tuple(name for name in names if name in DESKTOP_REPORT_CLEANUP_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_report_recent_command_helpers(bindings, recent_names)
    install_desktop_report_proof_command_helpers(bindings, proof_names)
    install_desktop_report_publish_command_helpers(bindings, publish_names)
    install_desktop_report_cleanup_command_helpers(bindings, cleanup_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
