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
    # The capture's DOM-path anchor for this element, derived from the
    # snapshot the way the importer derives `stable_anchor_id`.
    anchor: str | None = None
    # For a shape inside an inline `<svg>`: the outermost `<svg>` box, which
    # is the coordinate frame (viewBox mapping) its geometry is drawn in.
    svg_viewport: tuple[float, float, float, float] | None = None


def _has_area(box: tuple[float, float, float, float] | None) -> bool:
    return box is not None and len(box) == 4 and box[2] > 0 and box[3] > 0


def has_reference_box(node: BrowserNode) -> bool:
    """Whether Chromium laid this element out with a box geometry can measure.

    That is its own layout row with positive area, or, for a shape inside an
    inline `<svg>`, the frame it is drawn in. An element without one (a
    `<title>`, anything with no layout object) has no position to compare,
    so it can neither match a native box nor count against geometry coverage.
    """
    return _has_area(node.bounds) or _has_area(node.svg_viewport)


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

