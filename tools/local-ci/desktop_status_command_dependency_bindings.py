"""Dependency assembly for desktop status command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_status_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "desktop_receipt_for_fn": _binding(bindings, "desktop_receipt_for"),
        "desktop_capabilities_for_fn": _binding(bindings, "desktop_capabilities_for"),
        "desktop_optional_capabilities_fn": _binding(bindings, "desktop_optional_capabilities"),
        "desktop_run_manifests_fn": _binding(bindings, "desktop_run_manifests"),
        "desktop_run_summary_fn": _binding(bindings, "desktop_run_summary"),
        "desktop_proof_summaries_fn": _binding(bindings, "desktop_proof_summaries"),
        "normalize_desktop_optional_config_fn": _binding(bindings, "normalize_desktop_optional_config"),
        "desktop_target_contract_fn": _binding(bindings, "desktop_target_contract"),
        "desktop_publish_reports_fn": _binding(bindings, "desktop_publish_reports"),
        "desktop_status_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_status_lines"),
        "short_sha_fn": _binding(bindings, "short_sha"),
        "windows_tooling_detail_fn": _binding(bindings, "windows_tooling_detail"),
        "windows_repo_checkout_detail_fn": _binding(bindings, "windows_repo_checkout_detail"),
    }
