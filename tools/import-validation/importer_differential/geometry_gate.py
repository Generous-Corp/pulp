"""The geometry lens: registered panel geometry gated on a placement proof.

The observer places the panel-local native import at the authenticated
authored origin, so its render and layout are page-space like Chromium's.
The gate requires page-space geometry and registered panel geometry, both
sides moved back by that origin, together with a proof, from three
independent sources, that the panel-local native root sits at Chromium's
authored origin.
"""

from __future__ import annotations

import math
import statistics
from dataclasses import replace
from typing import Any

from .model import PROMOTION_THRESHOLDS, BrowserNode, LabError, NativeNode, has_reference_box
from .snapshot import DomSnapshot


def authenticated_authored_origin(
    capture: dict[str, Any], native_capture: dict[str, Any],
) -> tuple[float, float]:
    """Return the page-space origin of the authored panel both captures sign.

    Browser-backed native imports emit a panel-local root when the authored
    surface is smaller than the viewport, while Chromium's screenshot and DOM
    snapshot are page-space. The observer places the native panel at this
    origin so both sides are compared in page space. Only the authored-frame
    metadata carried by both captures is used, never a shift inferred from
    pixels or from an observed residual, and the two must agree.
    """
    try:
        authored = capture["reference"]["authored_frame"]
        native_authored = native_capture["reference"]["authored_frame"]
        frame_x = float(authored["x"])
        frame_y = float(authored["y"])
        native_frame_x = float(native_authored["x"])
        native_frame_y = float(native_authored["y"])
    except (KeyError, TypeError, ValueError) as exc:
        raise LabError("capture is missing authenticated authored-frame metadata") from exc
    if not all(math.isfinite(value) for value in
               (frame_x, frame_y, native_frame_x, native_frame_y)):
        raise LabError("authored-frame metadata is non-finite")
    if abs(frame_x - native_frame_x) > 1e-6 or abs(frame_y - native_frame_y) > 1e-6:
        raise LabError("browser/native authored-frame metadata disagrees")
    return frame_x, frame_y


def native_nodes_in_panel_space(
    nodes: list[NativeNode], placement_origin: tuple[float, float],
) -> list[NativeNode]:
    """Move page-placed native layout boxes back into the panel's own frame."""
    dx, dy = placement_origin
    if abs(dx) < 1e-9 and abs(dy) < 1e-9:
        return nodes
    return [
        replace(node, bounds=(
            (node.bounds[0] - dx, node.bounds[1] - dy, node.bounds[2], node.bounds[3])
            if node.bounds is not None else None))
        for node in nodes
    ]


def verify_native_authored_frame(
    ir: dict[str, Any], layout: dict[str, Any], authored_frame: dict[str, Any],
    tolerance: float = 1e-3,
    placement_origin: tuple[float, float] = (0.0, 0.0),
) -> dict[str, Any]:
    """Prove that the native authored panel is panel-local before cropping.

    `placement_origin` is where the observer placed the panel, as its render
    receipt records; the panel root's layout rect must sit exactly there.
    """
    root = ir.get("root")
    if not isinstance(root, dict):
        raise LabError("native DesignIR has no authored panel root")
    style = root.get("style")
    if not isinstance(style, dict):
        raise LabError("native authored panel root has no style bounds")
    try:
        actual = (0.0, 0.0, float(style["width"]), float(style["height"]))
        expected = (0.0, 0.0, float(authored_frame["width"]),
                    float(authored_frame["height"]))
    except (KeyError, TypeError, ValueError) as exc:
        raise LabError("native authored panel root has invalid style bounds") from exc
    # The imported DesignIR root is panel-local; the observer lays it out at
    # the placement origin. Its layout snapshot provides the panel root's
    # rect, required as a provenance check before accepting that contract.
    viewport_root = next((node for node in layout.get("nodes", [])
                          if node.get("id") == "browser:root"), None)
    if not isinstance(viewport_root, dict) or not isinstance(viewport_root.get("rect"), dict):
        raise LabError("native layout has no viewport root for registration proof")
    if any(abs(actual[index] - expected[index]) > tolerance for index in range(4)):
        raise LabError(
            "native authored panel bounds differ from authenticated frame: "
            f"expected {expected}, got {actual}")
    viewport_rect = viewport_root["rect"]
    try:
        viewport = tuple(float(viewport_rect[key]) for key in ("x", "y", "w", "h"))
    except (KeyError, TypeError, ValueError) as exc:
        raise LabError("native viewport root has invalid bounds") from exc
    if (abs(viewport[0] - placement_origin[0]) > tolerance
            or abs(viewport[1] - placement_origin[1]) > tolerance):
        raise LabError("native viewport root is not at the placement origin")
    return {
        "anchor": str(root.get("stable_anchor_id") or "browser:root"),
        "rect": list(actual),
        "viewport_rect": list(viewport),
        "placement_origin": list(placement_origin),
        "tolerance": tolerance,
        "origin_contract": "design-ir-root-at-panel-origin",
    }


def relative_error(reference: float, candidate: float) -> float:
    return abs(reference - candidate) / max(1.0, abs(reference))


def geometry_metrics(
    matches: list[tuple[BrowserNode, NativeNode, float]]
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    errors: list[float] = []
    browser_by_index = {browser.index: browser for browser, _native, _score in matches}

    def depth(index: int) -> int:
        seen: set[int] = set()
        value = 0
        while index >= 0 and index not in seen:
            seen.add(index)
            parent = browser_by_index.get(index)
            if parent is None or parent.parent < 0:
                break
            value += 1
            index = parent.parent
        return value

    for browser, native, _ in matches:
        # A native vector shape is laid out in its root `<svg>`'s frame, with
        # the path in viewBox coordinates, so its box is that frame and not
        # the shape's ink box. Compare it with the frame Chromium solved; the
        # shape's ink placement inside that frame is a visual-lens question.
        reference_frame = "element"
        reference_bounds = browser.bounds
        if native.node_type.lower() == "path" and browser.svg_viewport:
            reference_frame = "svg-viewport"
            reference_bounds = browser.svg_viewport
        if not reference_bounds or not native.bounds:
            continue
        labels = ("x", "y", "width", "height")
        signed_delta = [candidate - reference
                        for reference, candidate in zip(reference_bounds, native.bounds)]
        component_errors = [
            relative_error(reference, candidate)
            for reference, candidate in zip(reference_bounds, native.bounds)
        ]
        mean_error = statistics.fmean(component_errors)
        errors.append(mean_error)
        rows.append({
            "browser_node": browser.index,
            "native_anchor": native.anchor,
            "browser": dict(zip(labels, reference_bounds)),
            "candidate": dict(zip(labels, native.bounds)),
            "reference_frame": reference_frame,
            # Keep the direction visible. An absolute score can hide a
            # systematic offset by collapsing +N and -N into the same value.
            "signed_delta": dict(zip(labels, [round(v, 6) for v in signed_delta])),
            # `max` drops a NaN that is not its first argument, so a
            # non-finite edge is carried through explicitly.
            "max_abs_delta": (
                round(max(abs(v) for v in signed_delta), 6)
                if all(math.isfinite(v) for v in signed_delta) else math.nan),
            "depth": depth(browser.index),
            "mean_relative_error": round(mean_error, 6),
        })
    mean_error = statistics.fmean(errors) if errors else 1.0
    signed_columns = {
        label: [row["signed_delta"][label] for row in rows]
        for label in ("x", "y", "width", "height")
    }
    signed_summary = {
        label: {
            "mean": round(statistics.fmean(values), 6) if values else None,
            "median": round(statistics.median(values), 6) if values else None,
            "p95_abs": round(sorted(abs(value) for value in values)[
                min(len(values) - 1, max(0, math.ceil(len(values) * 0.95) - 1))], 6)
            if values else None,
            "min": min(values) if values else None,
            "max": max(values) if values else None,
        }
        for label, values in signed_columns.items()
    }
    root_indices = {browser.index for browser, _native, _score in matches
                    if browser.parent < 0}
    root_rows = [row for row in rows if row["browser_node"] in root_indices]
    result = {
        "score": round(max(0.0, 1.0 - mean_error), 6),
        "matched_bounds": len(rows),
        "mean_relative_error": round(mean_error, 6),
        "signed_delta_summary": signed_summary,
        "root_count": len(root_rows),
        "root_residual": root_rows[0] if len(root_rows) == 1 else None,
        "root_residuals": root_rows,
        "rows": rows,
    }
    return result


# Placement tolerance for the authored panel origin, and the largest per-box
# edge delta accepted in registered panel space. Both are CSS px. The measured
# browser-to-native floor on the differential corpus is below 0.004 px, so
# either bound is far above coordinate quantization.
FRAME_PLACEMENT_TOLERANCE_PX = 0.5


GEOMETRY_MAX_ABS_DELTA_PX = 1.0


NATIVE_HTML_ANCHOR = "capture:html[0]"


def dom_authored_surface(
    snapshot: DomSnapshot | dict[str, Any],
) -> dict[str, Any] | None:
    """Return the first occupying body child from the Chromium DOM snapshot.

    This mirrors the capture's authored-frame rule (first body element child
    with a positive layout box) but reads the DOMSnapshot layout tree, an
    artifact produced by a different CDP call than the capture's recorded
    authored frame. The box is the node's own row, the same row
    `browser_nodes` reads, so the proof checks the box the matched browser
    node carries rather than a later continuation row.
    """
    snap = DomSnapshot.coerce(snapshot)
    if not snap.has_document:
        return None
    types = snap.types
    body = next((index for index, node_type in enumerate(types)
                 if node_type == 1 and snap.tag(index) == "body"), None)
    if body is None:
        return None
    for index, parent in enumerate(snap.parents):
        if parent != body or index >= len(types) or types[index] != 1:
            continue
        layout_index = snap.own_row(index)
        if layout_index is None:
            continue
        bounds = snap.row_bounds(layout_index)
        if bounds is None or len(bounds) != 4:
            continue
        try:
            box = tuple(float(value) for value in bounds)
        except (TypeError, ValueError):
            continue
        if not all(math.isfinite(value) for value in box):
            continue
        if box[2] > 0 and box[3] > 0:
            return {"index": index, "bounds": box}
    return None


def frame_placement_check(
    snapshot: DomSnapshot | dict[str, Any], native_ir: dict[str, Any],
    native_layout: dict[str, Any],
    registered_matches: list[tuple[BrowserNode, NativeNode, float]],
    capture: dict[str, Any] | None = None,
    tolerance: float = FRAME_PLACEMENT_TOLERANCE_PX,
) -> dict[str, Any]:
    """Prove the observed native root sits at Chromium's authored origin.

    The observer places the panel-local import in page space, so the native
    layout is read as it was laid out: nothing is subtracted, and a panel the
    observer failed to move sits at the page origin and fails.

    Three sources must agree, and any missing source fails closed:

    - Chromium: the DOMSnapshot layout box of the authored root node (body's
      first occupying element child);
    - DesignIR: the origin recorded on the imported root;
    - native layout: the rect of the node matched to the authored root, which
      must be at the Chromium origin, and the rect of the html element, which
      must sit at the page origin.

    The capture envelope's authored frame is reported for context but is not
    part of the proof: DesignIR copies it, so comparing the two is
    metadata against metadata.
    """
    checks: dict[str, Any] = {}

    def floats(values: Any) -> tuple[float, ...] | None:
        try:
            result = tuple(float(value) for value in values)
        except (KeyError, TypeError, ValueError):
            return None
        return result if all(math.isfinite(value) for value in result) else None

    def compare(name: str, expected: tuple[float, float] | None,
                actual: tuple[float, float] | None) -> None:
        if expected is None or actual is None:
            checks[name] = {"ok": False, "reason": "unavailable"}
            return
        delta = (actual[0] - expected[0], actual[1] - expected[1])
        checks[name] = {
            "ok": all(abs(value) <= tolerance for value in delta),
            "expected": {"x": expected[0], "y": expected[1]},
            "actual": {"x": actual[0], "y": actual[1]},
            "signed_delta": {"x": round(delta[0], 6), "y": round(delta[1], 6)},
        }

    def layout_origin(anchor: str | None) -> tuple[float, float] | None:
        if anchor is None:
            return None
        node = next((item for item in native_layout.get("nodes", [])
                     if isinstance(item, dict) and item.get("id") == anchor), None)
        rect = node.get("rect") if isinstance(node, dict) else None
        if not isinstance(rect, dict) or "x" not in rect or "y" not in rect:
            return None
        return floats((rect["x"], rect["y"]))

    surface = dom_authored_surface(snapshot)
    dom_origin = surface["bounds"][:2] if surface else None
    checks["dom_authored_root_found"] = {
        "ok": surface is not None,
        "dom_node_index": surface["index"] if surface else None,
        "bounds": list(surface["bounds"]) if surface else None,
    }

    root = native_ir.get("root") if isinstance(native_ir, dict) else None
    attributes = root.get("attributes") if isinstance(root, dict) else None
    attributes = attributes if isinstance(attributes, dict) else {}
    recorded = (floats((attributes["browser_authored_frame_x"],
                        attributes["browser_authored_frame_y"]))
                if "browser_authored_frame_x" in attributes
                and "browser_authored_frame_y" in attributes else None)
    compare("design_ir_origin_matches_dom", dom_origin, recorded)

    native_root = None
    if surface is not None:
        native_root = next(
            (native for browser, native, _score in registered_matches
             if browser.index == surface["index"]), None)
    anchor = native_root.anchor if native_root else None
    checks["dom_authored_root_matched"] = {
        "ok": native_root is not None, "native_anchor": anchor}
    compare("native_root_layout_at_dom_origin", dom_origin, layout_origin(anchor))
    compare("native_html_layout_at_page_origin",
            (0.0, 0.0) if dom_origin else None,
            layout_origin(NATIVE_HTML_ANCHOR))

    frame = ((capture or {}).get("reference") or {}).get("authored_frame")
    envelope = (floats((frame["x"], frame["y"]))
                if isinstance(frame, dict) and "x" in frame and "y" in frame else None)
    return {
        "ok": all(row["ok"] for row in checks.values()),
        "tolerance_px": tolerance,
        "checks": checks,
        "context_not_counted": {
            "capture_envelope_origin": (
                {"x": envelope[0], "y": envelope[1]} if envelope else None),
            "reason": "DesignIR copies the capture envelope, so envelope "
                      "agreement is not placement evidence",
        },
    }


def geometry_metrics_with_registration(
    raw_matches: list[tuple[BrowserNode, NativeNode, float]],
    registered_matches: list[tuple[BrowserNode, NativeNode, float]],
    registration: dict[str, float],
    frame_placement: dict[str, Any] | None = None,
    max_abs_delta: float = GEOMETRY_MAX_ABS_DELTA_PX,
    *,
    reference_nodes: list[BrowserNode],
    placement: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Return raw page-space geometry and the registered geometry gate.

    ``score`` is page-space geometry: Chromium's boxes against the native
    layout as the observer placed it (``placement``, recorded as
    ``registration``). Its relative error divides by the page coordinate, so
    it is not monotone in defect size; it gates beside the registered checks
    rather than in place of them. ``gate`` is the geometry contract, and every
    part is required:

    - page-space mean: ``score`` at or above the geometry threshold, a
      non-finite score failing;
    - frame placement: ``frame_placement_check`` proves the panel origin from
      the DOM snapshot, DesignIR, and native layout;
    - registered mean score at or above the geometry threshold;
    - registered per-node maximum edge delta at most ``max_abs_delta`` CSS px
      on x, y, width, and height, with at least one matched box. A delta that
      is not a finite number is a violation, never a pass;
    - coverage: the measured boxes are at least the structural threshold's
      share of ``reference_nodes`` that have a box to measure
      (``has_reference_box``). The others, a `<title>` or any element with no
      layout object, can never be matched by position, so counting them would
      fail a fully matched page.

    The coverage floor exists because the delta bound is a claim about every
    box it measured and nothing else: one well-placed box among many
    unmatched ones would otherwise pass. The floor is the structural
    threshold, so geometry never certifies a smaller share of the reference
    than structure requires to be matched.

    Registration alone can never pass: a missing frame placement result fails.
    """
    geometry = geometry_metrics(raw_matches)
    geometry["role"] = "page-space-gated"
    geometry["registration"] = placement or {"x": 0.0, "y": 0.0, "source": "page-space"}
    registered = geometry_metrics(registered_matches)
    geometry["registered_geometry"] = registered
    geometry["registration_applied"] = registration

    violations = [
        {key: row[key] for key in ("browser_node", "native_anchor",
                                   "signed_delta", "max_abs_delta")}
        for row in registered["rows"]
        if not math.isfinite(row["max_abs_delta"])
        or row["max_abs_delta"] > max_abs_delta
    ]
    coverage_floor = PROMOTION_THRESHOLDS["structural"]
    reference_node_count = sum(1 for node in reference_nodes if has_reference_box(node))
    coverage = (registered["matched_bounds"] / reference_node_count
                if reference_node_count > 0 else 0.0)
    if frame_placement is None:
        frame_placement = {"ok": False, "reason": "not evaluated"}
    threshold = PROMOTION_THRESHOLDS["geometry"]
    checks = {
        "page_space_mean": {
            "ok": math.isfinite(geometry["score"]) and geometry["score"] >= threshold,
            "score": geometry["score"],
            "threshold": threshold,
        },
        "frame_placement": frame_placement,
        "registered_mean": {
            "ok": registered["score"] >= threshold,
            "score": registered["score"],
            "threshold": threshold,
        },
        "registered_max_abs_delta": {
            "ok": registered["matched_bounds"] > 0 and not violations,
            "limit_px": max_abs_delta,
            "max_abs_delta": max(
                (row["max_abs_delta"] for row in registered["rows"]), default=None),
            "matched_bounds": registered["matched_bounds"],
            "violation_count": len(violations),
            "violations": violations,
        },
        "registered_coverage": {
            "ok": coverage >= coverage_floor,
            "coverage": round(coverage, 6),
            "floor": coverage_floor,
            "matched_bounds": registered["matched_bounds"],
            "reference_node_count": reference_node_count,
            "unmeasurable_reference_count": len(reference_nodes) - reference_node_count,
        },
    }
    geometry["gate"] = {
        "ok": all(check["ok"] for check in checks.values()),
        "score": registered["score"],
        "matched_bounds": registered["matched_bounds"],
        "failed": [name for name, check in checks.items() if not check["ok"]],
        "checks": checks,
    }
    return geometry


def register_browser_nodes_to_native_frame(
    nodes: list[BrowserNode], capture: dict[str, Any],
    native_capture: dict[str, Any],
) -> tuple[list[BrowserNode], dict[str, float]]:
    """Translate authenticated page-space bounds into the native panel frame.

    Browser-backed native imports intentionally emit a panel-local root when
    the authored surface is smaller than the viewport.  Chromium's DOM
    snapshot still reports page-space bounds, so comparing those coordinates
    directly creates a constant root-margin residual.  Use only the signed
    authored-frame metadata carried by both captures; never infer a shift from
    pixels or from the observed residual.
    """
    frame_x, frame_y = authenticated_authored_origin(capture, native_capture)
    # The imported native root is panel-local at origin; the lab reads the
    # observer's page-placed layout back into that frame
    # (`native_nodes_in_panel_space`). Register page-space DOM bounds into it.
    dx = -frame_x
    dy = -frame_y
    if abs(dx) < 1e-9 and abs(dy) < 1e-9:
        return nodes, {"x": 0.0, "y": 0.0, "source": "authenticated-authored-frame"}
    def shifted(bounds: tuple[float, float, float, float] | None):
        if bounds is None:
            return None
        return (bounds[0] + dx, bounds[1] + dy, bounds[2], bounds[3])

    registered = [
        replace(node, bounds=shifted(node.bounds),
                svg_viewport=shifted(node.svg_viewport))
        for node in nodes
    ]
    return registered, {"x": round(dx, 6), "y": round(dy, 6),
                       "source": "authenticated-authored-frame"}


def geometry_gate_ok(geometry: dict[str, Any]) -> bool:
    """Absence of an evaluated geometry gate is a failure, never a pass."""
    gate = geometry.get("gate")
    return isinstance(gate, dict) and gate.get("ok") is True


def geometry_gate_failures(geometry: dict[str, Any]) -> list[str]:
    gate = geometry.get("gate")
    if not isinstance(gate, dict):
        return ["gate not evaluated"]
    return list(gate.get("failed", []))
