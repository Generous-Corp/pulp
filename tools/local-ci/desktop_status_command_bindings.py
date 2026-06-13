"""Bindings from the local_ci facade to desktop status command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


DESKTOP_STATUS_COMMAND_EXPORTS = ("cmd_desktop_status",)


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


def install_desktop_status_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_STATUS_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_STATUS_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
