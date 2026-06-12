"""Bindings from the local_ci facade to desktop artifact helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding


DESKTOP_ARTIFACT_EXPORTS = (
    "desktop_target_receipt_path",
    "desktop_receipt_for",
    "desktop_artifact_root",
    "create_desktop_run_bundle",
    "desktop_publish_root",
    "create_desktop_publish_bundle",
)


def desktop_target_receipt_path(bindings: dict, target_name: str) -> Path:
    return _binding(bindings, "_desktop_artifacts").desktop_target_receipt_path(
        target_name,
        desktop_receipts_dir_fn=_binding(bindings, "desktop_receipts_dir"),
    )


def desktop_receipt_for(bindings: dict, target_name: str) -> dict | None:
    return _binding(bindings, "_desktop_artifacts").desktop_receipt_for(
        target_name,
        desktop_target_receipt_path_fn=_binding(bindings, "desktop_target_receipt_path"),
    )


def desktop_artifact_root(bindings: dict, config: dict) -> Path:
    return _binding(bindings, "_desktop_artifacts").desktop_artifact_root(config)


def create_desktop_run_bundle(bindings: dict, config: dict, target_name: str, action: str) -> Path:
    return _binding(bindings, "_desktop_artifacts").create_desktop_run_bundle(config, target_name, action)


def desktop_publish_root(bindings: dict, config: dict) -> Path:
    return _binding(bindings, "_desktop_artifacts").desktop_publish_root(config)


def create_desktop_publish_bundle(bindings: dict, config: dict) -> Path:
    return _binding(bindings, "_desktop_artifacts").create_desktop_publish_bundle(config)
