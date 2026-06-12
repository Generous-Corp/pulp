"""Bindings from the local_ci facade to desktop doctor probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


DESKTOP_DOCTOR_PROBE_EXPORTS = (
    "desktop_doctor_checks",
    "probe_webdriver_endpoint",
)


def desktop_doctor_checks(bindings: Mapping[str, Any], config: dict, target_name: str) -> list[dict]:
    return _binding(bindings, "_desktop_doctor").desktop_doctor_checks(
        config,
        target_name,
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_target_contract_fn=_binding(bindings, "desktop_target_contract"),
        desktop_receipt_for_fn=_binding(bindings, "desktop_receipt_for"),
        macos_accessibility_trusted_fn=_binding(bindings, "macos_accessibility_trusted"),
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
        ssh_failure_detail_fn=_binding(bindings, "ssh_failure_detail"),
        probe_linux_launch_backend_fn=_binding(bindings, "probe_linux_launch_backend"),
        probe_linux_remote_tooling_fn=_binding(bindings, "probe_linux_remote_tooling"),
        probe_windows_session_agent_fn=_binding(bindings, "probe_windows_session_agent"),
        probe_windows_remote_tooling_fn=_binding(bindings, "probe_windows_remote_tooling"),
        probe_windows_repo_checkout_fn=_binding(bindings, "probe_windows_repo_checkout"),
        platform=_binding_attr(bindings, "sys", "platform"),
        which_fn=_binding(bindings, "shutil").which,
        probe_webdriver_endpoint_fn=_binding(bindings, "probe_webdriver_endpoint"),
    )


def probe_webdriver_endpoint(bindings: Mapping[str, Any], base_url: str, *, timeout: float = 5.0) -> dict:
    return _binding(bindings, "_desktop_doctor").probe_webdriver_endpoint(
        base_url,
        timeout=timeout,
        request_cls=_binding(bindings, "urllib").request.Request,
        urlopen_fn=_binding(bindings, "urllib").request.urlopen,
    )
