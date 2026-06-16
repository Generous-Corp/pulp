"""Desktop action interaction selector policy helpers."""

from __future__ import annotations


def desktop_interaction_requested(
    *,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
) -> bool:
    return any([click_point, click_view_id, click_view_type, click_view_text, click_view_label])


def desktop_click_selector(
    *,
    click_point: str | None = None,
    click_view_id: str | None = None,
    click_view_type: str | None = None,
    click_view_text: str | None = None,
    click_view_label: str | None = None,
    include_point: bool = True,
) -> dict:
    selector = {
        "id": click_view_id,
        "type": click_view_type,
        "text": click_view_text,
        "label": click_view_label,
    }
    if include_point:
        selector["point"] = click_point
    return selector


def pulp_app_interaction_summary(
    *,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
) -> dict:
    return {
        "mode": "pulp-app",
        "click": {
            "selector": desktop_click_selector(
                click_point=click_point,
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
            )
        },
    }
