"""Types and promotion policy shared by every stage of the differential lab."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

SCHEMA_VERSION = 1

PROMOTION_THRESHOLDS = {
    "structural": 0.99,
    "geometry": 0.98,
    "typography": 0.98,
    "visual": 0.995,
}


class LabError(RuntimeError):
    pass


@dataclass
class BrowserNode:
    index: int
    tag: str
    parent: int
    text: str
    attributes: dict[str, str]
    bounds: tuple[float, float, float, float] | None
    styles: dict[str, str]
    backend_node_id: int | None = None
    # For a shape inside an inline `<svg>`: the outermost `<svg>` box, which
    # is the coordinate frame (viewBox mapping) its geometry is drawn in.
    svg_viewport: tuple[float, float, float, float] | None = None


@dataclass
class NativeNode:
    anchor: str
    node_type: str
    name: str
    text: str
    parent_anchor: str | None
    style: dict[str, Any]
    bounds: tuple[float, float, float, float] | None
    backend_node_id: int | None = None
    # Set on a raster plane the importer hangs inside a captured element (a
    # canvas snapshot): the anchor of the element whose pixels it carries.
    captured_layer_of: str | None = None

