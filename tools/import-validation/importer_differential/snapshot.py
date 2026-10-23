"""Typed reader for Chromium's DOMSnapshot, the browser side of every lens.

The snapshot is a set of parallel arrays: node tables indexed by DOM node, a
layout table indexed by layout row, and a string table both index into.
Reading it is where the lenses used to disagree (which layout row is a node's
own box, how a style row maps onto property names), so every reader goes
through `DomSnapshot` and each of those rules lives here once.
"""

from __future__ import annotations

import re
from typing import Any

from .model import BrowserNode, LabError

# `layout.styles` rows are positional, so a row is unreadable without the
# request order that produced it. The capture writes that order into the
# snapshot as `computedStyleNames`; this list is only the shape of a snapshot
# predating that field. Keying off the snapshot is what stops a property added
# to the capture from shifting every later value here by one and mapping, say,
# a background into a border with no error anywhere.
LEGACY_COMPUTED_STYLES = [
    "display", "visibility", "opacity", "position", "z-index",
    "background-color", "background-image", "background-blend-mode",
    "border-top-color", "border-right-color", "border-bottom-color",
    "border-left-color", "border-top-width", "border-right-width",
    "border-bottom-width", "border-left-width", "border-top-style",
    "border-radius", "box-shadow", "text-shadow", "filter", "backdrop-filter",
    "transform", "transform-origin", "overflow", "clip-path", "mask-image",
    "mix-blend-mode", "isolation", "color", "font-family", "font-size",
    "font-weight", "font-style", "font-variation-settings", "text-align",
    "letter-spacing", "line-height", "text-transform",
    "text-decoration-line", "white-space", "cursor", "pointer-events",
]

# Elements that carry no box of their own in the comparison: the document
# wrappers and non-rendering metadata.
NON_RENDERED_TAGS = frozenset({
    "html", "head", "body", "script", "style", "link", "meta"})


def decode_string(strings: list[str], index: Any) -> str:
    if isinstance(index, int) and 0 <= index < len(strings):
        return strings[index]
    return ""


def principal_layout_rows(layout: dict[str, Any]) -> dict[int, int]:
    """Map each DOM node to the layout row that carries its own box.

    Chromium's DOMSnapshot can emit several layout rows for one DOM node: a
    pseudo-element's box is followed by its generated-content row, which
    reports a zero-size box at the page origin, and a split inline emits one
    row per continuation. The node's own box is the first row with positive
    area; a node whose rows are all empty keeps its first row so the empty
    box is still visible to the caller. The importer's C++ reader applies the
    same rule in `CapturedStyleIndex::own_layout_row`
    (tools/import-design/browser_capture_styles.hpp), and
    test/fixtures/browser-capture-layout-rows pins both to one answer.
    """
    raw_bounds = layout.get("bounds", [])
    rows: dict[int, int] = {}
    for layout_index, node_index in enumerate(layout.get("nodeIndex", [])):
        bounds = raw_bounds[layout_index] if layout_index < len(raw_bounds) else None
        has_area = (isinstance(bounds, list) and len(bounds) == 4
                    and all(isinstance(v, (int, float)) for v in bounds)
                    and bounds[2] > 0 and bounds[3] > 0)
        if node_index not in rows:
            rows[node_index] = layout_index
            continue
        current = raw_bounds[rows[node_index]] if rows[node_index] < len(raw_bounds) else None
        current_has_area = (isinstance(current, list) and len(current) == 4
                            and all(isinstance(v, (int, float)) for v in current)
                            and current[2] > 0 and current[3] > 0)
        if has_area and not current_has_area:
            rows[node_index] = layout_index
    return rows


class DomSnapshot:
    """The first document of a DOMSnapshot, addressed by DOM node index."""

    def __init__(self, raw: dict[str, Any]) -> None:
        self.raw = raw
        self.strings: list[str] = raw.get("strings", [])
        documents = raw.get("documents", [])
        self.has_document = bool(documents)
        document = documents[0] if documents else {}
        nodes = document.get("nodes", {})
        self.layout: dict[str, Any] = document.get("layout", {})
        self.parents: list[int] = nodes.get("parentIndex", [])
        self.types: list[int] = nodes.get("nodeType", [])
        self._names = nodes.get("nodeName", [])
        self._values = nodes.get("nodeValue", [])
        self._backend_node_ids = nodes.get("backendNodeId", [])
        self._attributes = nodes.get("attributes", [])
        self.style_names: list[str] = (
            raw.get("computedStyleNames") or LEGACY_COMPUTED_STYLES)
        self._own_rows = principal_layout_rows(self.layout)

    @classmethod
    def coerce(cls, snapshot: DomSnapshot | dict[str, Any]) -> DomSnapshot:
        return snapshot if isinstance(snapshot, DomSnapshot) else cls(snapshot)

    def tag(self, index: int) -> str:
        """Lowercase `nodeName`, or empty for an index past the table."""
        return decode_string(
            self.strings, self._names[index] if index < len(self._names) else None
        ).lower()

    def node_type(self, index: int) -> int | None:
        return self.types[index] if index < len(self.types) else None

    def parent(self, index: int) -> int:
        return self.parents[index] if index < len(self.parents) else -1

    def node_value(self, index: int) -> str:
        return decode_string(
            self.strings, self._values[index] if index < len(self._values) else None)

    def backend_node_id(self, index: int) -> int | None:
        ids = self._backend_node_ids
        if index < len(ids) and isinstance(ids[index], int) and ids[index] > 0:
            return int(ids[index])
        return None

    def attributes(self, index: int) -> dict[str, str]:
        pairs = self._attributes[index] if index < len(self._attributes) else []
        return {
            decode_string(self.strings, pairs[offset]):
                decode_string(self.strings, pairs[offset + 1])
            for offset in range(0, len(pairs) - 1, 2)}

    def own_row(self, index: int) -> int | None:
        """The layout row carrying this node's own box (see
        `principal_layout_rows`), or None when it produced no layout object."""
        return self._own_rows.get(index)

    def row_bounds(self, row: int) -> Any:
        """The raw `layout.bounds` entry for a layout row, unvalidated."""
        raw_bounds = self.layout.get("bounds", [])
        return raw_bounds[row] if row < len(raw_bounds) else None

    def bounds(self, index: int) -> tuple[float, float, float, float] | None:
        """The node's own box in page-space CSS px, or None without one."""
        row = self.own_row(index)
        if row is None:
            return None
        raw = self.row_bounds(row)
        if raw is None or len(raw) != 4:
            return None
        return tuple(float(v) for v in raw)

    def styles(self, index: int) -> dict[str, str]:
        """The node's computed styles from its own row, keyed by property.

        A row whose length disagrees with the declared property order is a
        decoding failure, never a short row: zip() would truncate in silence,
        which is precisely how a stale property list turns into
        shifted-by-one values that read as real style data.
        """
        row = self.own_row(index)
        raw_styles = self.layout.get("styles", [])
        if row is None or row >= len(raw_styles):
            return {}
        values = raw_styles[row]
        if len(values) != len(self.style_names):
            raise LabError(
                f"snapshot style row has {len(values)} "
                f"values but declares {len(self.style_names)} property "
                "names; the capture and this reader have drifted")
        return {
            name: decode_string(self.strings, value)
            for name, value in zip(self.style_names, values)
        }

    def form_control_theme(self) -> Any:
        """The capture's per-control theme verdicts, or None when absent."""
        return self.raw.get("formControlTheme")


def browser_nodes(snapshot: DomSnapshot | dict[str, Any]) -> list[BrowserNode]:
    snap = DomSnapshot.coerce(snapshot)
    if not snap.has_document:
        return []
    children: dict[int, list[int]] = {}
    for index, parent in enumerate(snap.parents):
        children.setdefault(parent, []).append(index)

    def descendant_text(index: int) -> str:
        parts: list[str] = []
        stack = list(children.get(index, []))
        while stack:
            child = stack.pop()
            if snap.node_type(child) == 3:
                value = snap.node_value(child)
                if value.strip():
                    parts.append(value.strip())
            stack.extend(children.get(child, []))
        return " ".join(reversed(parts)).strip()

    def inside_select(index: int) -> bool:
        cursor = snap.parent(index)
        for _ in range(len(snap.parents)):
            if cursor < 0:
                return False
            if snap.tag(cursor) == "select":
                return True
            cursor = snap.parent(cursor)
        return False

    def svg_viewport_of(index: int) -> tuple[float, float, float, float] | None:
        # The outermost `<svg>` ancestor, because nested viewports are drawn
        # inside their root's frame rather than as frames of their own.
        viewport = None
        seen: set[int] = set()
        cursor = snap.parent(index)
        while 0 <= cursor < len(snap.parents) and cursor not in seen:
            seen.add(cursor)
            if snap.tag(cursor) == "svg":
                bounds = snap.bounds(cursor)
                if bounds is not None:
                    viewport = bounds
            cursor = snap.parents[cursor]
        return viewport

    result: list[BrowserNode] = []
    for index, node_type in enumerate(snap.types):
        if node_type != 1:
            continue
        tag = snap.tag(index)
        if tag in NON_RENDERED_TAGS:
            continue
        attrs = snap.attributes(index)
        layout_index = snap.own_row(index)
        if layout_index is None and inside_select(index):
            # A closed select draws its selected option itself; its options
            # have no layout object and no box of their own to lower.
            continue
        bounds = snap.bounds(index)
        styles = snap.styles(index)
        if bounds and (bounds[2] <= 0 or bounds[3] <= 0):
            continue
        if styles.get("display") == "none" or styles.get("visibility") == "hidden":
            continue
        result.append(BrowserNode(
            index=index,
            tag=tag,
            parent=snap.parent(index),
            text=descendant_text(index),
            attributes=attrs,
            bounds=bounds,
            styles=styles,
            backend_node_id=snap.backend_node_id(index),
            svg_viewport=svg_viewport_of(index) if tag != "svg" else None,
        ))
    return result


# Styles under which Chromium composites the root or body differently from a
# flat fill of its background colour. The importer's own canvas rule refuses
# the same set (browser_capture_canvas_impl.hpp).
CANVAS_NEUTRAL_STYLES = {
    "opacity": "1",
    "filter": "none",
    "backdrop-filter": "none",
    "transform": "none",
    "clip-path": "none",
    "mask-image": "none",
    "mix-blend-mode": "normal",
    "background-image": "none",
}


def document_canvas_background(
    snapshot: DomSnapshot | dict[str, Any], capture: dict[str, Any],
) -> dict[str, Any]:
    """Resolve the colour Chromium paints across the page canvas.

    CSS propagates the root element's background to the canvas, and the
    body's when the root's is transparent. Only a flat, fully opaque colour on
    a box Chromium composites as a plain fill is resolved. Anything else --
    translucency, an image, a filter or blend, or two transparent boxes
    (whose UA canvas colour depends on the page's own color-scheme, which the
    capture does not record) -- is reported unresolved rather than
    approximated, so the observer leaves the page uncovered and the residual
    stays visible.
    """
    def unresolved(reason: str) -> dict[str, Any]:
        return {"status": "unresolved", "reason": reason, "color": None}

    snap = DomSnapshot.coerce(snapshot)
    if not snap.has_document:
        return unresolved("snapshot has no document")

    def element(tag: str, parent: int | None) -> dict[str, str] | None:
        for index in range(len(snap.types)):
            # Element nodes only: the doctype is also named "html".
            if snap.node_type(index) != 1 or snap.tag(index) != tag:
                continue
            if parent is not None and snap.parent(index) != parent:
                continue
            if snap.own_row(index) is None:
                return None
            styles = snap.styles(index)
            if not styles:
                return None
            styles["__index"] = str(index)
            return styles
        return None

    html = element("html", None)
    body = element("body", int(html["__index"])) if html else None
    if html is None or body is None:
        return unresolved("snapshot lacks a laid-out html/body pair")

    def flat_color(styles: dict[str, str]) -> tuple[str, float] | None:
        if any(styles.get(name, neutral) != neutral
               for name, neutral in CANVAS_NEUTRAL_STYLES.items()):
            return None
        match = re.fullmatch(
            r"rgba?\((\d+), (\d+), (\d+)(?:, ([0-9.]+))?\)",
            styles.get("background-color", ""))
        if not match:
            return None
        alpha = float(match.group(4)) if match.group(4) is not None else 1.0
        return f"rgb({match.group(1)}, {match.group(2)}, {match.group(3)})", alpha

    for label, styles in (("html", html), ("body", body)):
        resolved = flat_color(styles)
        if resolved is None:
            return unresolved(f"{label} is not a plain flat-colour fill")
        color, alpha = resolved
        if alpha == 1.0:
            return {"status": "resolved", "source": f"{label}-background",
                    "color": color}
        if alpha != 0.0:
            return unresolved(f"{label} background is translucent")
    return unresolved("html and body are transparent; the UA canvas colour "
                      "depends on a color-scheme the capture does not record")
