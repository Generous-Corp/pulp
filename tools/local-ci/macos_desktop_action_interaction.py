"""Interaction helpers for macOS desktop actions."""

from __future__ import annotations

from collections.abc import Callable


def macos_desktop_event_interaction_summary(
    *,
    window: dict,
    content_size: tuple[float, float],
    pid: int | None,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    view_tree: dict | None,
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    resolve_view_tree_click_point_fn: Callable[..., tuple[float, float]],
    screen_point_for_content_point_fn: Callable[[dict, tuple[float, float], tuple[float, float]], tuple[float, float]],
    activate_macos_pid_fn: Callable[[int], dict],
    dispatch_macos_click_fn: Callable[[float, float], dict],
    desktop_click_selector_fn: Callable[..., dict],
) -> dict:
    if click_point:
        content_point = parse_coordinate_pair_fn(click_point, flag_name="--click")
    else:
        content_point = resolve_view_tree_click_point_fn(
            view_tree or {},
            view_id=click_view_id,
            view_type=click_view_type,
            view_text=click_view_text,
            view_label=click_view_label,
        )
    screen_point = screen_point_for_content_point_fn(window, content_size, content_point)
    activation_payload = activate_macos_pid_fn(int(pid or 0)) if pid else {"activated": False}
    dispatch_payload = dispatch_macos_click_fn(*screen_point)
    return {
        "mode": "desktop-event",
        "click": {
            "content_point": {"x": content_point[0], "y": content_point[1]},
            "screen_point": {"x": screen_point[0], "y": screen_point[1]},
            "selector": desktop_click_selector_fn(
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
                include_point=False,
            ),
            "activation": activation_payload,
            "dispatch": dispatch_payload,
        },
    }
