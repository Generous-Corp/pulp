"""Linux desktop action interaction summary helpers."""

from __future__ import annotations

from collections.abc import Callable


def attach_linux_interaction_summary(
    manifest: dict,
    *,
    interaction_requested: bool,
    pulp_app_automation: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    pulp_app_interaction_summary_fn: Callable[..., dict],
) -> None:
    if not interaction_requested:
        return
    if pulp_app_automation:
        manifest["interaction"] = pulp_app_interaction_summary_fn(
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
        )
        return

    click_summary = {"point": click_point}
    if click_point:
        content_x, content_y = parse_coordinate_pair_fn(click_point, flag_name="--click")
        click_summary["content_point"] = {"x": content_x, "y": content_y}
    manifest["interaction"] = {"mode": "x11-window-driver", "click": click_summary}
