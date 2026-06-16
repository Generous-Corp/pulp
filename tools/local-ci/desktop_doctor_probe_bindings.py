"""Compatibility facade for desktop doctor probe dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_doctor_check_bindings import (
    DESKTOP_DOCTOR_CHECK_EXPORTS,
    desktop_doctor_checks,
    install_desktop_doctor_check_helpers,
)
from desktop_doctor_webdriver_probe_bindings import (
    DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS,
    install_desktop_doctor_webdriver_probe_helpers,
    probe_webdriver_endpoint,
)


DESKTOP_DOCTOR_PROBE_EXPORTS = (
    *DESKTOP_DOCTOR_CHECK_EXPORTS,
    *DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS,
)


def install_desktop_doctor_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_DOCTOR_PROBE_EXPORTS,
) -> None:
    check_names = tuple(name for name in names if name in DESKTOP_DOCTOR_CHECK_EXPORTS)
    webdriver_names = tuple(name for name in names if name in DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS)
    known_names = set(DESKTOP_DOCTOR_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_doctor_check_helpers(bindings, check_names)
    install_desktop_doctor_webdriver_probe_helpers(bindings, webdriver_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
