"""Bindings from the local_ci facade to desktop WebDriver probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS = ("probe_webdriver_endpoint",)


def probe_webdriver_endpoint(bindings: Mapping[str, Any], base_url: str, *, timeout: float = 5.0) -> dict:
    return _binding(bindings, "_desktop_doctor").probe_webdriver_endpoint(
        base_url,
        timeout=timeout,
        request_cls=_binding(bindings, "urllib").request.Request,
        urlopen_fn=_binding(bindings, "urllib").request.urlopen,
    )


def install_desktop_doctor_webdriver_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_DOCTOR_WEBDRIVER_PROBE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
