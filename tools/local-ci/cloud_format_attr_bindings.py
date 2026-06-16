"""Installer for cloud formatting helpers that remain direct module attributes."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs


CLOUD_FORMAT_EXPORTS = (
    "format_duration_secs",
    "format_memory_megabytes",
    "median_or_none",
    "print_cloud_field_detail",
    "recommend_cloud_provider",
    "render_selector_value",
    "summarize_runner_selector",
)


def install_cloud_format_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_FORMAT_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_cloud", names)
