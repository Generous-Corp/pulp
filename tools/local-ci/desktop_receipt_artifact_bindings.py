"""Bindings from the local_ci facade to desktop receipt artifact helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_RECEIPT_ARTIFACT_EXPORTS = (
    "desktop_target_receipt_path",
    "desktop_receipt_for",
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


def install_desktop_receipt_artifact_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_RECEIPT_ARTIFACT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
