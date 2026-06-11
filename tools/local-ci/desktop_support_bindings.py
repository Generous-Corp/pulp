"""Facade dependency bindings for generic desktop support helpers."""

from __future__ import annotations

from pathlib import Path


def _binding(bindings: dict, name: str):
    return bindings[name]


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


def resolve_desktop_target(bindings: dict, config: dict, target_name: str) -> dict:
    desktop_targets = config.get("desktop_automation", {}).get("targets", {})
    if target_name not in desktop_targets:
        raise ValueError(f"Unknown desktop target '{target_name}'.")
    target = desktop_targets[target_name]
    if not target.get("enabled", True):
        raise ValueError(f"Desktop target '{target_name}' is disabled.")
    return target


def desktop_optional_capabilities(bindings: dict, optional_cfg: dict | None) -> list[str]:
    return _binding(bindings, "_desktop_doctor").desktop_optional_capabilities(optional_cfg)


def desktop_capabilities_for(bindings: dict, adapter: str, tier: str, optional_cfg: dict | None = None) -> list[str]:
    return _binding(bindings, "_desktop_doctor").desktop_capabilities_for(adapter, tier, optional_cfg)


def desktop_check(bindings: dict, name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return _binding(bindings, "_desktop_doctor").desktop_check(name, ok, detail, required=required)


def check_writable_dir(bindings: dict, path: Path) -> tuple[bool, str]:
    return _binding(bindings, "_desktop_doctor").check_writable_dir(path)


def webdriver_status_url(bindings: dict, base_url: str) -> str:
    return _binding(bindings, "_desktop_doctor").webdriver_status_url(base_url)


def count_view_tree_nodes(bindings: dict, node: object) -> int:
    return _binding(bindings, "_desktop_actions").count_view_tree_nodes(node)


def parse_coordinate_pair(bindings: dict, value: str, *, flag_name: str) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").parse_coordinate_pair(value, flag_name=flag_name)


def iter_view_tree_nodes(bindings: dict, node: object, *, offset_x: float = 0.0, offset_y: float = 0.0):
    yield from _binding(bindings, "_desktop_actions").iter_view_tree_nodes(
        node,
        offset_x=offset_x,
        offset_y=offset_y,
    )


def resolve_view_tree_click_point(
    bindings: dict,
    view_tree: dict,
    *,
    view_id: str | None,
    view_type: str | None,
    view_text: str | None,
    view_label: str | None,
) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").resolve_view_tree_click_point(
        view_tree,
        view_id=view_id,
        view_type=view_type,
        view_text=view_text,
        view_label=view_label,
    )


def screen_point_for_content_point(
    bindings: dict,
    window: dict,
    content_size: tuple[float, float],
    content_point: tuple[float, float],
) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").screen_point_for_content_point(
        window,
        content_size,
        content_point,
    )


def default_desktop_label(bindings: dict, command: str | None, *, bundle_id: str | None = None) -> str:
    return _binding(bindings, "_desktop_actions").default_desktop_label(command, bundle_id=bundle_id)
