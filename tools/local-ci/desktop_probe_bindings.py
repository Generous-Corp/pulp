"""Compatibility facade for desktop probe dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_doctor_probe_bindings import (
    DESKTOP_DOCTOR_PROBE_EXPORTS,
    desktop_doctor_checks,
    install_desktop_doctor_probe_helpers,
    probe_webdriver_endpoint,
)
from desktop_windows_probe_bindings import (
    DESKTOP_WINDOWS_PROBE_EXPORTS,
    ensure_windows_remote_repo_checkout,
    ensure_windows_remote_tooling,
    install_desktop_windows_probe_helpers,
    install_windows_remote_tool,
    probe_windows_remote_tooling,
    probe_windows_repo_checkout,
    probe_windows_session_agent,
)


DESKTOP_PROBE_EXPORTS = (
    *DESKTOP_WINDOWS_PROBE_EXPORTS,
    *DESKTOP_DOCTOR_PROBE_EXPORTS,
)


def install_desktop_probe_helpers(bindings: dict[str, Any], names: tuple[str, ...] = DESKTOP_PROBE_EXPORTS) -> None:
    windows_names = tuple(name for name in names if name in DESKTOP_WINDOWS_PROBE_EXPORTS)
    doctor_names = tuple(name for name in names if name in DESKTOP_DOCTOR_PROBE_EXPORTS)
    known_names = set(DESKTOP_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_windows_probe_helpers(bindings, windows_names)
    install_desktop_doctor_probe_helpers(bindings, doctor_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
