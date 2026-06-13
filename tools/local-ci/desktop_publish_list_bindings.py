"""Bindings from the local_ci facade to desktop publish listing helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_PUBLISH_LIST_EXPORTS = (
    "desktop_publish_reports",
    "write_desktop_publish_rollups",
)


def desktop_publish_reports(bindings: Mapping[str, Any], config: dict, *, limit: int | None = None) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_publish_reports(
        config,
        limit=limit,
        desktop_publish_root_fn=_binding(bindings, "desktop_publish_root"),
    )


def write_desktop_publish_rollups(bindings: Mapping[str, Any], config: dict) -> None:
    return _binding(bindings, "_reporting").write_desktop_publish_rollups(
        config,
        desktop_publish_root_fn=_binding(bindings, "desktop_publish_root"),
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def install_desktop_publish_list_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_PUBLISH_LIST_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
