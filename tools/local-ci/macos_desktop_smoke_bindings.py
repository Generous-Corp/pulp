"""Bindings from the local_ci facade to macOS desktop smoke/action helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from macos_desktop_smoke_dependency_bindings import macos_desktop_smoke_dependencies


MACOS_DESKTOP_SMOKE_EXPORTS = ("run_macos_local_smoke",)


def run_macos_local_smoke(
    bindings: Mapping[str, Any],
    config: dict,
    command: str | None,
    *,
    action_name: str = "smoke",
    bundle_id: str | None,
    label: str | None,
    output_path: str | None,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    pulp_app_automation: bool = False,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
) -> dict:
    return bindings["_macos_desktop_action"].run_macos_local_smoke(
        config,
        command,
        action_name=action_name,
        bundle_id=bundle_id,
        label=label,
        output_path=output_path,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        pulp_app_automation=pulp_app_automation,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        source_request=source_request,
        **macos_desktop_smoke_dependencies(bindings),
    )


def install_macos_desktop_smoke_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_EXPORTS,
) -> None:
    known_names = set(MACOS_DESKTOP_SMOKE_EXPORTS)
    smoke_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), smoke_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
