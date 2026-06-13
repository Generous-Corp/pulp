"""Bindings from the local_ci facade to desktop publish staging helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_PUBLISH_STAGE_EXPORTS = ("stage_desktop_publish_report",)


def stage_desktop_publish_report(
    bindings: Mapping[str, Any],
    config: dict,
    manifests: list[dict],
    *,
    output_dir: Path | None = None,
    label: str | None = None,
) -> dict:
    return _binding(bindings, "_reporting").stage_desktop_publish_report(
        config,
        manifests,
        output_dir=output_dir,
        label=label,
        create_desktop_publish_bundle_fn=_binding(bindings, "create_desktop_publish_bundle"),
        now_iso_fn=_binding(bindings, "now_iso"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        write_desktop_publish_rollups_fn=_binding(bindings, "write_desktop_publish_rollups"),
        publish_report_to_branch_fn=_binding(bindings, "publish_report_to_branch"),
    )


def install_desktop_publish_stage_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_PUBLISH_STAGE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
