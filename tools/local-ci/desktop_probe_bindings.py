"""Bindings from the local_ci facade to desktop probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def probe_windows_repo_checkout(bindings: Mapping[str, Any], host: str, repo_path: str | None) -> dict:
    return _binding(bindings, "_windows_probe").probe_windows_repo_checkout(
        host,
        repo_path,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_repo_path_is_unsafe_fn=_binding(bindings, "windows_repo_path_is_unsafe"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def ensure_windows_remote_repo_checkout(
    bindings: Mapping[str, Any],
    host: str,
    repo_path: str | None,
    *,
    remote_url: str | None = None,
    bundle_name: str | None = None,
    bundle_ref: str | None = None,
) -> dict:
    return _binding(bindings, "_windows_probe").ensure_windows_remote_repo_checkout(
        host,
        repo_path,
        remote_url=remote_url,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        probe_windows_repo_checkout_fn=_binding(bindings, "probe_windows_repo_checkout"),
        windows_repo_path_is_unsafe_fn=_binding(bindings, "windows_repo_path_is_unsafe"),
        windows_default_repo_checkout_path_fn=_binding(bindings, "windows_default_repo_checkout_path"),
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def probe_windows_session_agent(bindings: Mapping[str, Any], host: str, contract: dict) -> dict:
    return _binding(bindings, "_windows_probe").probe_windows_session_agent(
        host,
        contract,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def probe_windows_remote_tooling(bindings: Mapping[str, Any], host: str) -> dict:
    return _binding(bindings, "_windows_probe").probe_windows_remote_tooling(
        host,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
    )


def install_windows_remote_tool(bindings: Mapping[str, Any], host: str, package_id: str, *, timeout: int = 900) -> None:
    return _binding(bindings, "_windows_probe").install_windows_remote_tool(
        host,
        package_id,
        timeout=timeout,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def ensure_windows_remote_tooling(bindings: Mapping[str, Any], host: str, *, install_optional: bool = False) -> dict:
    return _binding(bindings, "_windows_probe").ensure_windows_remote_tooling(
        host,
        install_optional=install_optional,
        required_tools=_binding(bindings, "WINDOWS_REQUIRED_REMOTE_TOOLS"),
        optional_tools=_binding(bindings, "WINDOWS_OPTIONAL_REMOTE_TOOLS"),
        probe_windows_remote_tooling_fn=_binding(bindings, "probe_windows_remote_tooling"),
        install_windows_remote_tool_fn=_binding(bindings, "install_windows_remote_tool"),
    )


def desktop_doctor_checks(bindings: Mapping[str, Any], config: dict, target_name: str) -> list[dict]:
    return _binding(bindings, "_desktop_doctor").desktop_doctor_checks(
        config,
        target_name,
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_target_contract_fn=_binding(bindings, "desktop_target_contract"),
        desktop_receipt_for_fn=_binding(bindings, "desktop_receipt_for"),
        macos_accessibility_trusted_fn=_binding(bindings, "macos_accessibility_trusted"),
        probe_macos_screencapture_fn=_binding(bindings, "probe_macos_screencapture"),
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
        ssh_failure_detail_fn=_binding(bindings, "ssh_failure_detail"),
        probe_linux_launch_backend_fn=_binding(bindings, "probe_linux_launch_backend"),
        probe_linux_remote_tooling_fn=_binding(bindings, "probe_linux_remote_tooling"),
        probe_windows_session_agent_fn=_binding(bindings, "probe_windows_session_agent"),
        probe_windows_remote_tooling_fn=_binding(bindings, "probe_windows_remote_tooling"),
        probe_windows_repo_checkout_fn=_binding(bindings, "probe_windows_repo_checkout"),
        platform=_binding(bindings, "sys").platform,
        which_fn=_binding(bindings, "shutil").which,
        resolve_ffmpeg_path_fn=_binding(bindings, "resolve_ffmpeg_path"),
        probe_macos_avfoundation_screen_fn=_binding(bindings, "probe_macos_avfoundation_screen"),
        probe_webdriver_endpoint_fn=_binding(bindings, "probe_webdriver_endpoint"),
    )


def probe_webdriver_endpoint(bindings: Mapping[str, Any], base_url: str, *, timeout: float = 5.0) -> dict:
    return _binding(bindings, "_desktop_doctor").probe_webdriver_endpoint(
        base_url,
        timeout=timeout,
        request_cls=_binding(bindings, "urllib").request.Request,
        urlopen_fn=_binding(bindings, "urllib").request.urlopen,
    )
