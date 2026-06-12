"""Bindings from the local_ci facade to desktop management command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_MANAGEMENT_COMMAND_EXPORTS = (
    "cmd_desktop_status",
    "cmd_desktop_config_show",
    "cmd_desktop_config_set",
    "cmd_desktop_recent",
    "cmd_desktop_proof",
    "cmd_desktop_publish",
    "cmd_desktop_cleanup",
)


def cmd_desktop_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_status(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_receipt_for_fn=_binding(bindings, "desktop_receipt_for"),
        desktop_capabilities_for_fn=_binding(bindings, "desktop_capabilities_for"),
        desktop_optional_capabilities_fn=_binding(bindings, "desktop_optional_capabilities"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        normalize_desktop_optional_config_fn=_binding(bindings, "normalize_desktop_optional_config"),
        desktop_target_contract_fn=_binding(bindings, "desktop_target_contract"),
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
        desktop_status_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_status_lines"),
        short_sha_fn=_binding(bindings, "short_sha"),
        windows_tooling_detail_fn=_binding(bindings, "windows_tooling_detail"),
        windows_repo_checkout_detail_fn=_binding(bindings, "windows_repo_checkout_detail"),
    )


def cmd_desktop_config_show(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config_show(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_config_show_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_config_show_lines"),
    )


def cmd_desktop_config_set(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config_set(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        save_config_fn=_binding(bindings, "save_config"),
        config_path_fn=_binding(bindings, "config_path"),
        normalize_publish_mode_fn=_binding(bindings, "normalize_publish_mode"),
        parse_config_bool_fn=_binding(bindings, "parse_config_bool"),
        normalize_desktop_config_fn=_binding(bindings, "normalize_desktop_config"),
        desktop_config_update_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_config_update_lines"),
    )


def cmd_desktop_recent(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_recent(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_recent_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_recent_lines"),
        short_sha_fn=_binding(bindings, "short_sha"),
    )


def cmd_desktop_proof(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_proof(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        desktop_proof_empty_line_fn=_binding_attr(bindings, "_desktop_cli", "desktop_proof_empty_line"),
        desktop_proof_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_proof_lines"),
        short_sha_fn=_binding(bindings, "short_sha"),
    )


def cmd_desktop_publish(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_publish(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        stage_desktop_publish_report_fn=_binding(bindings, "stage_desktop_publish_report"),
        desktop_publish_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_publish_lines"),
    )


def cmd_desktop_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_cleanup(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        prune_desktop_run_manifests_fn=_binding(bindings, "prune_desktop_run_manifests"),
        write_desktop_run_rollups_fn=_binding(bindings, "write_desktop_run_rollups"),
        desktop_cleanup_empty_line_fn=_binding_attr(bindings, "_desktop_cli", "desktop_cleanup_empty_line"),
        desktop_cleanup_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_cleanup_lines"),
    )


def install_desktop_management_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
