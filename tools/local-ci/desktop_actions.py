"""Desktop automation action helper policy for local CI."""

from __future__ import annotations

from pathlib import Path
import shlex


def desktop_action_artifact_paths(bundle_dir: Path, output_path: str | None = None) -> dict[str, Path]:
    return {
        "screenshot": Path(output_path).expanduser() if output_path else bundle_dir / "screenshots" / "window.png",
        "before_screenshot": bundle_dir / "screenshots" / "before.png",
        "diff_screenshot": bundle_dir / "screenshots" / "diff.png",
        "ui_snapshot": bundle_dir / "ui-tree.json",
        "video": bundle_dir / "video" / "proof.mp4",
        "video_audio": bundle_dir / "video" / "audio.wav",
        "video_composed": bundle_dir / "video" / "proof-composed.mp4",
        "video_issue": bundle_dir / "video" / "proof.issue.mp4",
        "video_small": bundle_dir / "video" / "proof.small.mp4",
        "video_metadata": bundle_dir / "video" / "metadata.json",
        "video_composed_metadata": bundle_dir / "video" / "composed-metadata.json",
        "video_issue_metadata": bundle_dir / "video" / "issue-metadata.json",
        "video_small_metadata": bundle_dir / "video" / "small-metadata.json",
        "video_poster": bundle_dir / "video" / "poster.png",
        "stdout": bundle_dir / "stdout.log",
        "stderr": bundle_dir / "stderr.log",
    }


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


def count_view_tree_nodes(node: object) -> int:
    if not isinstance(node, dict):
        return 0
    total = 1
    children = node.get("children")
    if isinstance(children, list):
        total += sum(count_view_tree_nodes(child) for child in children)
    return total


def parse_coordinate_pair(value: str, *, flag_name: str) -> tuple[float, float]:
    parts = [segment.strip() for segment in value.split(",", 1)]
    if len(parts) != 2:
        raise ValueError(f"{flag_name} must be in X,Y form.")
    try:
        return float(parts[0]), float(parts[1])
    except ValueError as exc:
        raise ValueError(f"{flag_name} must contain numeric X,Y values.") from exc


def iter_view_tree_nodes(node: object, *, offset_x: float = 0.0, offset_y: float = 0.0):
    if not isinstance(node, dict):
        return
    bounds = node.get("bounds") if isinstance(node.get("bounds"), dict) else {}
    absolute_x = offset_x + float(bounds.get("x", 0.0) or 0.0)
    absolute_y = offset_y + float(bounds.get("y", 0.0) or 0.0)
    absolute_bounds = {
        "x": absolute_x,
        "y": absolute_y,
        "width": float(bounds.get("width", 0.0) or 0.0),
        "height": float(bounds.get("height", 0.0) or 0.0),
    }
    yield node, absolute_bounds
    children = node.get("children")
    if isinstance(children, list):
        for child in children:
            yield from iter_view_tree_nodes(child, offset_x=absolute_x, offset_y=absolute_y)


def resolve_view_tree_click_point(
    view_tree: dict,
    *,
    view_id: str | None,
    view_type: str | None,
    view_text: str | None,
    view_label: str | None,
) -> tuple[float, float]:
    for node, bounds in iter_view_tree_nodes(view_tree):
        if not node.get("visible", True):
            continue
        if view_id and node.get("id") != view_id:
            continue
        if view_type and node.get("type") != view_type:
            continue
        if view_text and node.get("text") != view_text:
            continue
        if view_label and node.get("label") != view_label:
            continue
        if bounds["width"] <= 0 or bounds["height"] <= 0:
            continue
        return bounds["x"] + (bounds["width"] / 2.0), bounds["y"] + (bounds["height"] / 2.0)
    filters = [
        part for part in [
            f"id={view_id}" if view_id else None,
            f"type={view_type}" if view_type else None,
            f"text={view_text}" if view_text else None,
            f"label={view_label}" if view_label else None,
        ] if part
    ]
    joined = ", ".join(filters) or "<none>"
    raise RuntimeError(f"No visible view matched click selector ({joined}).")


def view_tree_inspector_summary(view_tree: dict) -> dict:
    return {
        "root_id": view_tree.get("id"),
        "root_type": view_tree.get("type"),
        "view_count": count_view_tree_nodes(view_tree),
    }


def content_size_from_window(window: dict) -> tuple[float, float]:
    bounds = window.get("bounds", {})
    return (
        float(bounds.get("width", 0.0) or 0.0),
        float(bounds.get("height", 0.0) or 0.0),
    )


def content_size_from_view_tree(
    view_tree: dict,
    fallback: tuple[float, float],
) -> tuple[float, float]:
    root_bounds = view_tree.get("bounds") if isinstance(view_tree.get("bounds"), dict) else {}
    return (
        float(root_bounds.get("width", fallback[0]) or fallback[0]),
        float(root_bounds.get("height", fallback[1]) or fallback[1]),
    )


def screen_point_for_content_point(
    window: dict,
    content_size: tuple[float, float],
    content_point: tuple[float, float],
) -> tuple[float, float]:
    bounds = window.get("bounds", {})
    window_x = float(bounds.get("x", 0.0) or 0.0)
    window_y = float(bounds.get("y", 0.0) or 0.0)
    window_width = float(bounds.get("width", 0.0) or 0.0)
    window_height = float(bounds.get("height", 0.0) or 0.0)
    content_width, content_height = content_size
    point_x, point_y = content_point

    inset_x = max((window_width - content_width) / 2.0, 0.0)
    inset_y = max(window_height - content_height, 0.0)
    return window_x + inset_x + point_x, window_y + inset_y + point_y


def default_desktop_label(command: str | None, *, bundle_id: str | None = None) -> str:
    if bundle_id:
        return bundle_id.split(".")[-1] or bundle_id
    args = shlex.split(command or "")
    if not args:
        return "desktop-run"
    return Path(args[0]).stem or "desktop-run"
