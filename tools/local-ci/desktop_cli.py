"""Desktop automation CLI line helpers for local CI."""

from __future__ import annotations


def _append_image_change_lines(lines: list[str], image_change: dict) -> None:
    lines.append(f"  image_change: changed={image_change.get('changed')} method={image_change.get('method')}")
    bbox = image_change.get("bbox")
    if bbox:
        lines.append(f"  image_change_bbox: {bbox['left']},{bbox['top']} -> {bbox['right']},{bbox['bottom']}")


def desktop_action_success_lines(action: str, target_name: str, manifest: dict) -> list[str]:
    lines = [
        f"Desktop {action} PASS for `{target_name}`",
        f"  label: {manifest['label']}",
        f"  pid: {manifest['pid']}",
    ]
    artifacts = manifest["artifacts"]
    if action in {"smoke", "click"}:
        if artifacts.get("before_screenshot"):
            lines.append(f"  before_screenshot: {artifacts['before_screenshot']}")
        if artifacts.get("diff_screenshot"):
            lines.append(f"  diff_screenshot: {artifacts['diff_screenshot']}")
        if artifacts.get("image_change"):
            _append_image_change_lines(lines, artifacts["image_change"])
    lines.append(f"  screenshot: {artifacts['screenshot']}")
    if artifacts.get("ui_snapshot"):
        lines.append(f"  ui_snapshot: {artifacts['ui_snapshot']}")
    if action in {"smoke", "click"} and manifest.get("interaction"):
        interaction = manifest["interaction"]
        if interaction.get("mode"):
            lines.append(f"  interaction_mode: {interaction['mode']}")
        click = interaction.get("click", {})
        screen_point = click.get("screen_point") or {}
        if "x" in screen_point and "y" in screen_point:
            lines.append(f"  click_screen_point: {screen_point.get('x')},{screen_point.get('y')}")
    lines.append(f"  bundle: {artifacts['bundle_dir']}")
    return lines
