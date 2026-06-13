"""Installer for cloud record-store helpers that remain direct module attributes."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs


CLOUD_RECORD_STORE_EXPORTS = (
    "cloud_record_sort_key",
    "cloud_run_path",
    "enrich_cloud_record_provider_metadata",
    "filter_cloud_records",
    "find_cloud_record",
    "load_cloud_record",
    "load_result",
    "normalize_cloud_record",
    "refresh_cloud_record",
    "save_cloud_record",
    "update_cloud_record_from_run",
)


def install_cloud_record_store_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_RECORD_STORE_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_cloud", names)
