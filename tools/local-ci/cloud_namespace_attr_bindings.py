"""Installer for cloud Namespace helpers that remain direct module attributes."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs


CLOUD_NAMESPACE_EXPORTS = (
    "infer_job_os",
    "normalize_github_timestamp",
    "normalize_namespace_instance",
    "nsc_available",
    "nsc_instance_history",
    "nsc_logged_in",
    "nsc_run",
    "nsc_version",
    "nsc_workspace_info",
    "parse_colon_separated_fields",
    "parse_optional_bool",
    "print_namespace_setup_help",
)


def install_cloud_namespace_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_NAMESPACE_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_cloud", names)
