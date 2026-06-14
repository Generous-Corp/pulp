"""Windows desktop action interaction summary helpers."""

from __future__ import annotations

from collections.abc import Callable


def attach_windows_interaction_summary(
    manifest: dict,
    *,
    remote_manifest: dict,
    interaction_requested: bool,
    pulp_app_automation: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    pulp_app_interaction_summary_fn: Callable[..., dict],
) -> None:
    remote_interaction = remote_manifest.get("interaction")
    if remote_interaction:
        manifest["interaction"] = remote_interaction
    elif interaction_requested:
        manifest["interaction"] = pulp_app_interaction_summary_fn(
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
        )
        if not pulp_app_automation:
            manifest["interaction"]["mode"] = "window-capture"
