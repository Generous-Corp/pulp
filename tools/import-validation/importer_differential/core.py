#!/usr/bin/env python3
"""Chromium-oracle comparison core; it never changes canonical import output."""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import shutil
import stat
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# The lens modules' public names are re-exported here: callers and tests
# address the whole lab as `importer_differential.core`.
from .form_controls import attribute_residual, form_controls, support_boundaries
from .geometry_gate import (
    FRAME_PLACEMENT_TOLERANCE_PX,
    GEOMETRY_MAX_ABS_DELTA_PX,
    NATIVE_HTML_ANCHOR,
    authenticated_authored_origin,
    dom_authored_surface,
    frame_placement_check,
    geometry_gate_failures,
    geometry_gate_ok,
    geometry_metrics,
    geometry_metrics_with_registration,
    native_nodes_in_panel_space,
    register_browser_nodes_to_native_frame,
    relative_error,
    verify_native_authored_frame,
)
from .aggregate import CORPUS_SCHEMA, aggregate_reports, percentile
from .model import (
    PROMOTION_THRESHOLDS,
    SCHEMA_VERSION,
    BrowserNode,
    LabError,
    NativeNode,
)
from .native_transform import IDENTITY, Affine, node_transform, transformed_bounds
from .recognition import source_recognition, unsupported_features
from .reports import format_summary
from .snapshot import (
    CANVAS_NEUTRAL_STYLES,
    LEGACY_COMPUTED_STYLES,
    DomSnapshot,
    browser_nodes,
    decode_string,
    document_canvas_background,
    principal_layout_rows,
)
from .verdicts import (
    OBSERVER_COUNTER_SCHEMA,
    ROOT_CAUSE_ORDER,
    classify_gaps,
    promotion_recommendation,
    work_counter_gate,
)
from .visual import (
    classify_full_frame,
    full_frame_ok,
    outside_frame_ink,
    score_images,
    visual_metrics,
)

REPORT_SCHEMA = "pulp-importer-differential-report-v1"
OBSERVABILITY_SCHEMA = "pulp-importer-observability-v1"
MAX_SNAPSHOT_BYTES = 256 * 1024 * 1024
TIMING_BUDGET_KEYS = (
    "browser_import_ms", "native_import_ms", "native_render_ms", "native_total_ms",
)


TYPOGRAPHY_MAP = {
    "font-family": "fontFamily",
    "font-size": "fontSize",
    "font-weight": "fontWeight",
    "font-style": "fontStyle",
    "text-align": "textAlign",
    "letter-spacing": "letterSpacing",
    "line-height": "lineHeight",
    "text-transform": "textTransform",
    "text-decoration-line": "textDecoration",
    "white-space": "whiteSpace",
    "color": "color",
}

# Native DesignIR intentionally omits CSS properties whose authored value is
# the CSS initial/inherited default. Treat those omissions as equivalent only
# for the documented defaults; an authored non-default value must still be
# present and equal in the native style.
CSS_INITIAL_EQUIVALENTS = {
    "font-style": {"normal"},
    # "left" equals the initial "start" only for left-to-right content; an RTL
    # fixture would need "right" here instead.
    "text-align": {"start", "left"},
    "letter-spacing": {"normal", "0px"},
    "line-height": {"normal"},
    "text-transform": {"none"},
    "text-decoration-line": {"none"},
    "white-space": {"normal"},
}


def timing_budget_result(
    timings: dict[str, Any], budgets_ms: dict[str, int] | None,
) -> dict[str, Any]:
    """Compare measured importer timings with caller-supplied budgets.

    Budgets are deliberately opt-in and apply only to this headless lab. They
    do not turn a readback into TTFP, TTNI, or IFNF evidence.
    """
    if not budgets_ms:
        return {"status": "not-requested", "budgets_ms": {}, "violations": []}
    violations = [
        {"metric": key, "value_ms": timings.get(key), "budget_ms": budget}
        for key, budget in budgets_ms.items()
        if (not isinstance(timings.get(key), (int, float))
            or isinstance(timings.get(key), bool)
            or not math.isfinite(timings[key])
            or timings[key] < 0 or timings[key] > budget)
    ]
    return {
        "status": "fail" if violations else "pass",
        "budgets_ms": dict(budgets_ms),
        "violations": violations,
    }


@dataclass
class CommandResult:
    argv: list[str]
    elapsed_ms: int
    returncode: int
    stdout: str
    stderr: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def interaction_plan_sha256(path: Path) -> str:
    """Match interaction_plan.mjs's redacted identity without a Node runtime."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        actions = document["actions"]
        if document["schema"] != "pulp-browser-interactions-v1" or document["version"] != 1:
            raise ValueError
        normalized = []
        for candidate in actions:
            action = candidate["action"]
            if action == "wait-ms":
                normalized.append({"action": "wait-ms", "milliseconds": candidate["milliseconds"]})
                continue
            item = {
                "action": action,
                "selector": candidate["selector"],
                "timeout_ms": candidate.get("timeout_ms", 5000),
            }
            expected = candidate.get("expect")
            if expected is not None:
                item["expect"] = {
                    "selector": expected["selector"],
                    "state": expected.get("state", "visible"),
                    "timeout_ms": expected.get("timeout_ms", 5000),
                }
            if action == "type":
                # JavaScript String.length counts UTF-16 code units.
                item["text_length"] = len(candidate["text"].encode("utf-16-le")) // 2
            elif action == "dispatch-event":
                item["event"] = candidate["event"]
            elif action == "wait-for":
                item["state"] = candidate.get("state", "visible")
            normalized.append(item)
        payload = json.dumps(
            {"schema": "pulp-browser-interactions-v1", "version": 1, "actions": normalized},
            separators=(",", ":"), ensure_ascii=False,
        ).encode("utf-8")
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise LabError("browser interaction plan could not be canonically authenticated") from exc
    return hashlib.sha256(payload).hexdigest()


# Settle-loop polling counts in a capture envelope's `provenance.settle`.
SETTLE_COUNTER_KEYS = frozenset({"rounds", "stable_rounds"})


def canonical_json_sha256(source: Path | bytes) -> str:
    """Hash JSON evidence after removing documented per-run envelope fields.

    Capture and IR receipts retain elapsed/import timestamps for diagnostics.
    Those fields must not make an otherwise identical campaign appear
    nondeterministic. Browser-capture envelopes also omit their exact snapshot
    binding here: CDP frame IDs vary per capture. Each raw binding is verified
    separately; `snapshot_semantic_sha256` covers the snapshot's content.
    They also omit the settle loop's iteration counters
    (`provenance.settle.rounds` and `stable_rounds`, and an IR's top-level
    `settle_rounds`): how many polls the page took to go quiet depends on host
    load, not on what was captured. Only those counters are scrubbed (the
    settle record's only in capture envelopes); every other settle field and
    every content field still enters the digest.
    """
    volatile_keys = {"elapsed_ms", "imported_at"}
    document = json.loads(source if isinstance(source, bytes) else source.read_bytes())
    capture_envelope = isinstance(document, dict) and document.get("schema") == "pulp-browser-capture-v1"

    def scrub(value: Any, path: tuple[str | int, ...] = ()) -> Any:
        if isinstance(value, dict):
            # Raw DOM bytes contain capture-local CDP frame IDs. This digest
            # was historically envelope/output determinism, not semantic DOM
            # equivalence. Retain and verify raw snapshot bindings separately.
            document_entry = (capture_envelope and len(path) == 2
                              and path[0] == "documents" and isinstance(path[1], int))
            settle_record = capture_envelope and path == ("provenance", "settle")
            return {
                key: scrub(item, (*path, key))
                for key, item in value.items()
                if key not in volatile_keys
                and not (document_entry and key == "snapshot_sha256")
                and not (settle_record and key in SETTLE_COUNTER_KEYS)
                and not (path == () and key == "settle_rounds")
            }
        if isinstance(value, list):
            return [scrub(item, (*path, index)) for index, item in enumerate(value)]
        return value

    canonical = json.dumps(
        scrub(document),
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


_SESSION_TOKEN_KEYS = {"loaderId": "<loader>", "backendNodeId": "<backend-node>"}


def snapshot_semantic_sha256(snapshot_bytes: bytes) -> str:
    """Hash a DOM snapshot with only its capture-local CDP identifiers fixed.

    `canonical_json_sha256` drops a capture envelope's snapshot binding because
    the raw snapshot carries a CDP frame ID minted per browser session, so a
    DOM change, a different form-control theme verdict, or any other content
    inside the snapshot moved no determinism digest. This one covers the whole
    snapshot. Each document's `frameId` string is replaced by its document
    position, and any `loaderId` or `backendNodeId` value by a constant; they
    are opaque per-run tokens that name no content (Chromium hands out backend
    node ids in a run-dependent order). Everything else, including the string
    table and the capture's `formControlTheme` report, enters the digest
    unchanged.
    """
    document = json.loads(snapshot_bytes)
    if not isinstance(document, dict):
        raise LabError("DOM snapshot is not a JSON object")
    strings = list(document.get("strings") or [])
    documents = document.get("documents") or []
    for position, entry in enumerate(documents):
        if not isinstance(entry, dict):
            continue
        frame = entry.get("frameId")
        if isinstance(frame, int) and 0 <= frame < len(strings):
            strings[frame] = f"<frame:{position}>"

    def scrub(value: Any) -> Any:
        if isinstance(value, dict):
            return {key: (_SESSION_TOKEN_KEYS[key] if key in _SESSION_TOKEN_KEYS
                          else scrub(item))
                    for key, item in value.items()}
        if isinstance(value, list):
            return [scrub(item) for item in value]
        return value

    canonical = json.dumps(
        scrub({**document, "strings": strings}),
        sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=False) + "\n", encoding="utf-8")


def run_command(argv: list[str], timeout_seconds: int) -> CommandResult:
    start = time.monotonic()
    try:
        result = subprocess.run(
            argv,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_seconds,
            check=False,
            encoding="utf-8",
        )
    except subprocess.TimeoutExpired as exc:
        elapsed = int((time.monotonic() - start) * 1000)
        # The partial output on a timeout is bytes even under text=True, and
        # every reader of a CommandResult expects str.
        def partial(value: bytes | str | None) -> str:
            if isinstance(value, bytes):
                return value.decode("utf-8", errors="replace")
            return value or ""
        return CommandResult(
            argv, elapsed, 124, partial(exc.stdout),
            partial(exc.stderr) or "command timed out")
    elapsed = int((time.monotonic() - start) * 1000)
    return CommandResult(
        argv, elapsed, result.returncode, result.stdout, result.stderr)


def command_record(result: CommandResult) -> dict[str, Any]:
    def public_arg(value: str) -> str:
        # Keep command provenance useful without leaking machine-local paths.
        # Flags remain exact; path-like values are reduced to their basename.
        return Path(value).name if "/" in value else value

    timing_line = next(
        (line.strip() for line in result.stdout.splitlines()
         if line.strip().startswith("✓ imported ")),
        None,
    )
    return {
        "elapsed_ms": result.elapsed_ms,
        "exit_code": result.returncode,
        "executable": Path(result.argv[0]).name if result.argv else "",
        "argv": [public_arg(value) for value in result.argv],
        "stdout_sha256": hashlib.sha256(result.stdout.encode()).hexdigest(),
        "stderr_sha256": hashlib.sha256(result.stderr.encode()).hexdigest(),
        "import_timing_line": timing_line,
    }


def native_nodes(ir: dict[str, Any], layout: dict[str, Any]) -> list[NativeNode]:
    layout_by_anchor: dict[str, tuple[float, float, float, float]] = {}
    for view in layout.get("views", []):
        anchor = view.get("anchor_id")
        if not anchor or anchor in layout_by_anchor:
            continue
        try:
            layout_by_anchor[anchor] = tuple(
                float(view[key]) for key in ("x", "y", "width", "height"))
        except (KeyError, TypeError, ValueError):
            continue
    for node in layout.get("nodes", []):
        anchor = node.get("id")
        rect = node.get("rect", {})
        if not anchor or anchor in layout_by_anchor:
            continue
        try:
            layout_by_anchor[anchor] = tuple(
                float(rect[key]) for key in ("x", "y", "w", "h"))
        except (KeyError, TypeError, ValueError):
            continue
    result: list[NativeNode] = []

    def visit(node: dict[str, Any], parent: str | None,
              parent_captured: bool = False, parent_matrix: Affine = IDENTITY) -> None:
        anchor = str(node.get("stable_anchor_id") or "")
        attributes = node.get("attributes") if isinstance(
            node.get("attributes"), dict) else {}
        node_type = str(node.get("type") or "unknown")
        text = str(node.get("content") or node.get("text") or "").strip()
        style = node.get("style") if isinstance(node.get("style"), dict) else {}
        box = layout_by_anchor.get(anchor)
        matrix = node_transform(parent_matrix, style.get("transform"), box)
        result.append(NativeNode(
            anchor=anchor,
            node_type=node_type,
            name=str(node.get("name") or ""),
            text=text,
            parent_anchor=parent,
            style=style,
            bounds=transformed_bounds(matrix, box),
            backend_node_id=(
                int(node.get("attributes", {}).get("backend_node_id"))
                if isinstance(node.get("attributes"), dict)
                and str(node.get("attributes", {}).get("backend_node_id", "")).isdigit()
                else None),
            captured_layer_of=(
                parent if parent_captured and "captured_layer" in attributes
                else None),
        ))
        for child in node.get("children", []):
            if isinstance(child, dict):
                visit(child, anchor or parent, "captured_element" in attributes, matrix)

    root = ir.get("root")
    if isinstance(root, dict):
        visit(root, None)
    return result


def node_match_score(browser: BrowserNode, native: NativeNode) -> float:
    score = 0.0
    if (browser.backend_node_id is not None
            and browser.backend_node_id == native.backend_node_id):
        score += 100.0
    browser_id = browser.attributes.get("id", "")
    if browser_id and browser_id in {native.anchor, native.name}:
        score += 5.0
    if browser.text and native.text:
        if browser.text == native.text:
            score += 4.0
        elif browser.text in native.text or native.text in browser.text:
            score += 2.0
    if browser.tag == native.name.lower():
        score += 2.0
    type_tags = {
        "text": {"p", "span", "label", "h1", "h2", "h3", "h4", "h5", "h6"},
        "button": {"button"},
        "image": {"img", "svg"},
        "frame": {"div", "main", "section", "article", "nav", "header", "footer"},
    }
    if browser.tag in type_tags.get(native.node_type.lower(), set()):
        score += 1.5
    return score


def has_identity_signal(browser: BrowserNode, native: NativeNode) -> bool:
    if browser.backend_node_id is not None and native.backend_node_id is not None:
        # Both captures have an authoritative DOM identity. Never let a
        # same-text/same-tag heuristic hide a mismatched capture node.
        return browser.backend_node_id == native.backend_node_id
    browser_id = browser.attributes.get("id", "")
    if browser_id and browser_id in {native.anchor, native.name}:
        return True
    if browser.text and native.text and (
        browser.text == native.text
        or browser.text in native.text
        or native.text in browser.text
    ):
        return True
    return False


def match_nodes(
    browser: list[BrowserNode], native: list[NativeNode]
) -> tuple[list[tuple[BrowserNode, NativeNode, float]], list[BrowserNode], list[NativeNode]]:
    candidates: list[tuple[float, int, int]] = []
    for browser_index, browser_node in enumerate(browser):
        for native_index, native_node in enumerate(native):
            score = node_match_score(browser_node, native_node)
            if score >= 1.5 and has_identity_signal(browser_node, native_node):
                candidates.append((score, browser_index, native_index))
    candidates.sort(reverse=True)
    used_browser: set[int] = set()
    used_native: set[int] = set()
    matches: list[tuple[BrowserNode, NativeNode, float]] = []
    for score, browser_index, native_index in candidates:
        if browser_index in used_browser or native_index in used_native:
            continue
        used_browser.add(browser_index)
        used_native.add(native_index)
        matches.append((browser[browser_index], native[native_index], score))
    missing = [
        node for index, node in enumerate(browser) if index not in used_browser]
    extra = [node for index, node in enumerate(native) if index not in used_native]
    return matches, missing, extra


def comparable_native_nodes(native: list[NativeNode]) -> tuple[list[NativeNode], list[NativeNode]]:
    """Split native nodes into browser-element comparables and representation wrappers.

    Chromium's DOM snapshot reader intentionally exposes element nodes only;
    native DesignIR also contains synthetic frame roots, html/body capture
    wrappers, explicit text children, and the raster plane inside a captured
    canvas. Counting those implementation nodes as unmatched browser elements
    makes an exact element join look like a structural failure. Keep them in
    the report as ignored representation nodes, but exclude them from
    precision/recall.
    """
    ignored_names = {"html", "body"}
    comparable: list[NativeNode] = []
    ignored: list[NativeNode] = []
    for node in native:
        # A captured element's raster plane is that element's paint, the way
        # a text run is its element's text. `captured_layer_of` is set only
        # for a plane hanging directly inside its captured element, so a plane
        # whose element was dropped still counts.
        layer_of_present_element = node.captured_layer_of is not None
        if (node.node_type.lower() == "text"
                or node.anchor == "browser:root"
                or node.name.lower() in ignored_names
                or layer_of_present_element):
            ignored.append(node)
        else:
            comparable.append(node)
    return comparable, ignored


def structural_comparison(
    browser_flat: list[BrowserNode], native_flat: list[NativeNode]
) -> dict[str, Any]:
    """Join browser elements to comparable native nodes and score the join.

    Matching runs against comparable native nodes only. A representation node
    (a text child, the browser root, an html/body wrapper) is excluded from the
    precision denominator, so it must not be able to supply a match either:
    a browser element whose only counterpart is a bare text node was lost in
    lowering and counts as missing. Precision and recall therefore share one
    population and neither can exceed 1.
    """
    comparable_native, ignored_native = comparable_native_nodes(native_flat)
    matches, missing, extra = match_nodes(browser_flat, comparable_native)
    matched_parent_by_browser = {
        browser.index: native.anchor for browser, native, _ in matches}
    parentage_errors = []
    for browser, native, _ in matches:
        expected_parent = matched_parent_by_browser.get(browser.parent)
        if expected_parent is not None and native.parent_anchor != expected_parent:
            parentage_errors.append({
                "browser_node": browser.index,
                "native_anchor": native.anchor,
                "expected_native_parent": expected_parent,
                "actual_native_parent": native.parent_anchor,
            })
    structurally_valid_matches = max(0, len(matches) - len(parentage_errors))
    recall = structurally_valid_matches / max(1, len(browser_flat))
    precision = structurally_valid_matches / max(1, len(comparable_native))
    score = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {
        "matches": matches, "missing": missing, "extra": extra,
        "comparable_native": comparable_native, "ignored_native": ignored_native,
        "parentage_errors": parentage_errors,
        "structurally_valid_matches": structurally_valid_matches,
        "precision": precision, "recall": recall, "score": score,
    }


def viewport_dpr_audit(
    capture: dict[str, Any], native_capture: dict[str, Any],
    expected_logical_size: tuple[int, int], actual_render_size: tuple[int, int],
) -> dict[str, Any]:
    """Record the coordinate contract used by a comparison.

    The audit is diagnostic, but it is deliberately strict about missing or
    contradictory provenance. A signed geometry residual is not actionable if
    the two captures used different roots, viewport extents, or device scale.
    """
    reference = capture.get("reference")
    native_reference = native_capture.get("reference")
    resolved = capture.get("provenance", {}).get("viewport", {}).get("resolved")
    native_resolved = native_capture.get("provenance", {}).get("viewport", {}).get("resolved")
    if not all(isinstance(item, dict) for item in (reference, native_reference,
                                                    resolved, native_resolved)):
        raise LabError("capture is missing object-shaped reference or viewport provenance")
    try:
        raw_dpr = reference["device_scale_factor"]
        raw_native_dpr = native_reference["device_scale_factor"]
        if isinstance(raw_dpr, bool) or isinstance(raw_native_dpr, bool):
            raise ValueError("boolean device scale")
        dpr = float(raw_dpr)
        native_dpr = float(raw_native_dpr)
        logical = (int(reference["logical_width"]), int(reference["logical_height"]))
    except (KeyError, TypeError, ValueError) as exc:
        raise LabError("capture is missing a valid logical extent or device scale") from exc
    if (not math.isfinite(dpr) or not math.isfinite(native_dpr)
            or dpr <= 0 or native_dpr <= 0):
        raise LabError("capture device scale factor must be positive")
    resolved_size = (resolved.get("width"), resolved.get("height"))
    native_resolved_size = (native_resolved.get("width"), native_resolved.get("height"))
    expected_pixels = (round(logical[0] * dpr), round(logical[1] * dpr))
    checks = {
        # Chromium can capture overflow beyond the requested viewport. The
        # resolved viewport is the request; the reference extent may be larger.
        "resolved_viewport_matches_request": resolved_size == expected_logical_size,
        "logical_extent_covers_request": logical[0] >= expected_logical_size[0]
        and logical[1] >= expected_logical_size[1],
        "native_logical_size_matches_browser": native_reference.get("logical_width") == logical[0]
        and native_reference.get("logical_height") == logical[1],
        "dpr_matches_native": dpr == native_dpr,
        "resolved_viewport_matches_native": resolved_size == native_resolved_size,
        "render_size_matches_dpr": actual_render_size == expected_pixels,
    }
    return {
        "schema": "pulp-importer-viewport-dpr-audit-v1",
        "logical_size": list(logical),
        "requested_viewport": list(expected_logical_size),
        "resolved_viewport": [resolved_size[0], resolved_size[1]],
        "device_scale_factor": dpr,
        "native_device_scale_factor": native_dpr,
        "expected_pixel_size": list(expected_pixels),
        "actual_render_size": list(actual_render_size),
        "checks": checks,
        "ok": all(checks.values()),
    }


def normalize_css_value(value: Any) -> str:
    if value is None:
        return ""
    text = str(value).strip().lower()
    if re.fullmatch(r"-?\d+(?:\.\d+)?", text):
        text += "px"
    return re.sub(r"\s+", " ", text)


def typography_metrics(
    matches: list[tuple[BrowserNode, NativeNode, float]]
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    equal = 0
    comparable = 0
    for browser, native, _ in matches:
        if not browser.text and native.node_type.lower() != "text":
            continue
        differences: list[dict[str, str]] = []
        for browser_key, native_key in TYPOGRAPHY_MAP.items():
            browser_value = normalize_css_value(browser.styles.get(browser_key))
            native_value = normalize_css_value(native.style.get(native_key))
            if not browser_value:
                continue
            comparable += 1
            if (native_value and browser_value == native_value) or (
                    not native_value
                    and browser_value in CSS_INITIAL_EQUIVALENTS.get(
                        browser_key, set())):
                equal += 1
            else:
                differences.append({
                    "property": browser_key,
                    "browser": browser_value,
                    "candidate": native_value or "<missing>",
                })
        rows.append({
            "browser_node": browser.index,
            "native_anchor": native.anchor,
            "text": browser.text or native.text,
            "differences": differences,
        })
    return {
        "score": round(equal / comparable, 6) if comparable else 0.0,
        "comparable_properties": comparable,
        "equal_properties": equal,
        "rows": rows,
    }


def form_control_residual(
    browser_flat_raw: list[BrowserNode],
    reference_path: Path,
    candidate_path: Path,
    visual: dict[str, Any],
    snapshot: DomSnapshot | dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Form controls, their support status, and their share of the residual.

    Attributed on the image pair the visual gate scored: the registered
    authored-frame crops when registration ran, the full frames otherwise.
    """
    controls = form_controls(browser_flat_raw, snapshot)
    if not controls:
        return {"controls": [], "support_boundaries": [],
                "foreground_error_total": 0.0,
                "boundary_foreground_error_share": 0.0,
                "scored_surface": None}
    try:
        from PIL import Image
    except ImportError as exc:
        raise LabError("Pillow is required for form-control attribution") from exc
    reference = Image.open(reference_path).convert("RGBA")
    candidate = Image.open(candidate_path).convert("RGBA")
    registered = visual.get("registered_visual")
    scale = float(visual["registration"].get("device_scale_factor", 1.0))
    origin = (0.0, 0.0)
    surface = "full-frame"
    if registered:
        registration = registered["registration"]
        reference = reference.crop(tuple(registration["browser_crop_px"]))
        candidate = candidate.crop(tuple(registration["native_crop_px"]))
        frame = registration["logical_frame"]
        origin = (float(frame["x"]), float(frame["y"]))
        surface = "registered-authored-frame"
    attribution = attribute_residual(controls, reference, candidate, origin, scale)
    attribution["support_boundaries"] = support_boundaries(attribution)
    attribution["scored_surface"] = surface
    return attribution


def find_browser_evidence(browser_ir: Path) -> Path:
    evidence = browser_ir.with_name(browser_ir.stem + "-browser-capture")
    if not evidence.is_dir():
        raise LabError(f"browser evidence directory not found: {evidence}")
    return evidence


def capture_logical_size(capture: dict[str, Any]) -> tuple[int, int]:
    reference = capture.get("reference", {})
    try:
        width = int(reference["logical_width"])
        height = int(reference["logical_height"])
    except (KeyError, TypeError, ValueError) as exc:
        raise LabError("browser capture has no valid logical extent") from exc
    if width <= 0 or height <= 0:
        raise LabError("browser capture has no valid logical extent")
    return width, height


def panel_covers_page(capture: dict[str, Any]) -> bool:
    """Whether the authored frame is the whole captured page."""
    width, height = capture_logical_size(capture)
    try:
        frame = capture["reference"]["authored_frame"]
        x, y = float(frame["x"]), float(frame["y"])
        w, h = float(frame["width"]), float(frame["height"])
    except (KeyError, TypeError, ValueError):
        return False
    return x <= 0 and y <= 0 and x + w >= width and y + h >= height


def import_lane_args(
    importer: Path,
    source_args: list[str],
    output_ir: Path,
    viewport_args: list[str],
    interaction_args: list[str],
    browser_path: Path | None,
    *,
    native: bool,
) -> list[str]:
    """The importer argv for one lane of a comparison.

    Both lanes import the same authored source at the same viewport with the
    same interaction plan and browser; the native lane only adds
    --native-panel-lowering. Neither passes --offline, which would substitute
    the legacy partial parser for the authored source.
    """
    args = [str(importer), *source_args, "--mode", "baked", "--emit", "ir-json",
            "--output", str(output_ir)]
    if native:
        args.append("--native-panel-lowering")
    args.extend([*viewport_args, *interaction_args])
    if browser_path:
        args.extend(["--browser", str(browser_path)])
    return args


def verify_capture_binding(
    capture: dict[str, Any], native_capture: dict[str, Any], source_sha256: str,
    *, expected_interaction_plan_sha256: str | None = None,
) -> None:
    """Fail closed unless both captures came from the same authored input.

    Each capture's source hash must equal the authored file's, and the native
    lowering must have run at the browser capture's viewport and in the same
    browser; otherwise the comparison measures two different inputs.
    """
    browser = capture.get("provenance", {})
    native = native_capture.get("provenance", {})
    if not isinstance(browser, dict) or not isinstance(native, dict):
        raise LabError("capture provenance is malformed")
    browser_source = browser.get("source", {})
    native_source = native.get("source", {})
    if not isinstance(browser_source, dict) or not isinstance(native_source, dict):
        raise LabError("capture source provenance is malformed")
    if browser_source.get("sha256") != source_sha256:
        raise LabError("browser capture source hash is not bound to the authored input")
    if native_source.get("sha256") != source_sha256:
        raise LabError("native lowering source hash is not bound to the authored input")
    if native.get("viewport") != browser.get("viewport"):
        raise LabError("browser and native lowering viewport provenance differs")
    if native.get("browser") != browser.get("browser"):
        raise LabError("browser and native lowering browser provenance differs")
    browser_reference = capture.get("reference", {})
    native_reference = native_capture.get("reference", {})
    if not isinstance(browser_reference, dict) or not isinstance(native_reference, dict):
        raise LabError("capture reference provenance is malformed")
    browser_dpr = browser_reference.get("device_scale_factor")
    native_dpr = native_reference.get("device_scale_factor")
    if (not isinstance(browser_dpr, (int, float)) or isinstance(browser_dpr, bool)
            or browser_dpr <= 0):
        raise LabError("browser capture device scale factor is missing or invalid")
    if (not isinstance(native_dpr, (int, float)) or isinstance(native_dpr, bool)
            or native_dpr <= 0):
        raise LabError("native lowering device scale factor is missing or invalid")
    if browser_dpr != native_dpr:
        raise LabError("browser and native lowering device scale factor differs")
    browser_interactions = browser.get("interactions")
    native_interactions = native.get("interactions")
    if (browser_interactions is None) != (native_interactions is None):
        raise LabError("browser and native lowering interaction provenance is incomplete")
    if browser_interactions != native_interactions:
        raise LabError("browser and native lowering interaction provenance differs")
    if browser_interactions is not None:
        if not isinstance(browser_interactions, dict):
            raise LabError("interaction provenance is malformed")
        plan_identity = browser_interactions.get("plan_sha256")
        if (not isinstance(plan_identity, str)
                or not re.fullmatch(r"[0-9a-f]{64}", plan_identity)):
            raise LabError("interaction provenance plan hash is malformed")
    if expected_interaction_plan_sha256 is not None:
        if not re.fullmatch(r"[0-9a-f]{64}", expected_interaction_plan_sha256):
            raise LabError("requested interaction plan hash is malformed")
        browser_plan = browser_interactions.get("plan_sha256") if isinstance(browser_interactions, dict) else None
        native_plan = native_interactions.get("plan_sha256") if isinstance(native_interactions, dict) else None
        if (not isinstance(browser_plan, str) or not re.fullmatch(r"[0-9a-f]{64}", browser_plan)
                or not isinstance(native_plan, str) or not re.fullmatch(r"[0-9a-f]{64}", native_plan)
                or browser_plan != expected_interaction_plan_sha256
                or native_plan != expected_interaction_plan_sha256):
            raise LabError("capture interaction provenance is not bound to the requested plan")


def verify_capture_image_binding(capture: dict[str, Any], image_path: Path) -> None:
    """Fail closed when the capture envelope does not name the PNG we compare."""
    reference = capture.get("reference", {})
    assets = capture.get("assets", [])
    if not isinstance(reference, dict) or not isinstance(assets, list):
        raise LabError("capture image provenance is malformed")
    asset_id = reference.get("asset_id")
    asset = next((item for item in assets if isinstance(item, dict) and item.get("id") == asset_id), None)
    if not isinstance(asset, dict):
        raise LabError("capture reference asset is missing")
    expected_sha = asset.get("sha256")
    if not isinstance(expected_sha, str) or expected_sha != sha256_file(image_path):
        raise LabError("capture reference PNG digest does not match capture envelope")
    if (not isinstance(asset.get("width_px"), int) or isinstance(asset.get("width_px"), bool)
            or not isinstance(asset.get("height_px"), int) or isinstance(asset.get("height_px"), bool)):
        raise LabError("capture reference PNG dimensions are missing from capture envelope")
    try:
        from PIL import Image
        with Image.open(image_path) as image:
            expected_size = (asset.get("width_px"), asset.get("height_px"))
            if image.size != expected_size:
                raise LabError("capture reference PNG dimensions do not match capture envelope")
    except ImportError as exc:
        raise LabError("Pillow is required to verify capture PNG dimensions") from exc


def verify_capture_snapshot_binding(
    capture: dict[str, Any], evidence: Path, *, required: bool = False,
) -> tuple[dict[str, Any], bytes]:
    """Verify producer-declared snapshot bytes; never upgrade legacy evidence.

    Existing captures can still support diagnostic analysis without a digest.
    A document-background experiment must request the producer binding. This
    proves byte identity against the envelope, not a signature or paint parity.
    Return the verified buffer so the caller parses and retains those bytes.
    """
    if not isinstance(capture, dict):
        raise LabError("capture snapshot envelope is malformed")
    documents = capture.get("documents", [])
    if not isinstance(documents, list):
        raise LabError("capture snapshot documents are malformed")
    if documents and (len(documents) != 1 or not isinstance(documents[0], dict)
                      or not isinstance(documents[0].get("snapshot_asset"), str)
                      or not documents[0]["snapshot_asset"]):
        raise LabError("capture snapshot requires exactly one document with a snapshot asset")
    declared = [item for item in documents
                if isinstance(item, dict) and "snapshot_sha256" in item]
    snapshot = evidence / "dom-snapshot.json"
    def read_snapshot() -> bytes:
        try:
            if not snapshot.resolve().is_relative_to(evidence.resolve()):
                raise LabError("capture snapshot sidecar escapes evidence directory")
            info = snapshot.stat()
            if not stat.S_ISREG(info.st_mode):
                raise LabError("capture snapshot sidecar is not a regular file")
            if info.st_size > MAX_SNAPSHOT_BYTES:
                raise LabError("capture snapshot sidecar exceeds 256 MiB")
            with snapshot.open("rb") as stream:
                info = os.fstat(stream.fileno())
                if not stat.S_ISREG(info.st_mode):
                    raise LabError("capture snapshot sidecar is not a regular file")
                if info.st_size > MAX_SNAPSHOT_BYTES:
                    raise LabError("capture snapshot sidecar exceeds 256 MiB")
                snapshot_bytes = stream.read(info.st_size + 1)
                if len(snapshot_bytes) != info.st_size:
                    raise LabError("capture snapshot sidecar changed during bounded read")
                return snapshot_bytes
        except OSError as exc:
            raise LabError("capture snapshot sidecar is unavailable") from exc

    if not declared:
        if required:
            raise LabError("capture snapshot has no producer digest")
        return {"status": "legacy-unbound", "sha256": None}, read_snapshot()
    # The current capture producer emits one document and one DOM snapshot.
    # Do not silently choose a document if a different envelope is supplied.
    if len(documents) != 1 or len(declared) != 1:
        raise LabError("capture snapshot requires exactly one document")
    document = declared[0]
    if document.get("snapshot_asset") != "dom-snapshot.json":
        raise LabError("capture snapshot requires a dom-snapshot.json document")
    expected = document["snapshot_sha256"]
    if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise LabError("capture snapshot producer digest is malformed")
    snapshot_bytes = read_snapshot()
    actual = hashlib.sha256(snapshot_bytes).hexdigest()
    if actual != expected:
        raise LabError("capture snapshot digest does not match capture envelope")
    return {"status": "producer-bound", "sha256": actual}, snapshot_bytes


@dataclass
class LaneOutputs:
    """Both importer lanes and the native render, run and verified.

    Every field here has passed its binding check: the captures are bound to
    the authored source, viewport, browser, and interaction plan; the PNG and
    snapshot bytes are bound to their envelopes; the render receipt proves a
    Skia render; and the viewport/DPR audit passed.
    """
    browser_dir: Path
    native_dir: Path
    comparison_dir: Path
    browser_ir: Path
    native_ir: Path
    native_render: Path
    native_layout: Path
    native_receipt: Path
    browser_run: CommandResult
    native_run: CommandResult
    render_run: CommandResult
    browser_command: dict[str, Any]
    native_command: dict[str, Any]
    render_command: dict[str, Any]
    browser_doc: dict[str, Any]
    native_doc: dict[str, Any]
    evidence: Path
    native_evidence: Path
    capture: dict[str, Any]
    native_capture: dict[str, Any]
    capture_bytes: bytes
    native_capture_bytes: bytes
    browser_snapshot_binding: dict[str, Any]
    native_snapshot_binding: dict[str, Any]
    browser_snapshot_bytes: bytes
    native_snapshot_bytes: bytes
    render_receipt: dict[str, Any]
    source_sha256: str
    capture_size: tuple[int, int]
    viewport_dpr: dict[str, Any]
    # Where the observer placed the panel-local native import (the signed
    # authored origin, which its receipt records) and the page colour it
    # painted around it.
    placement_origin: tuple[float, float]
    page_background: dict[str, Any]


@dataclass
class Scores:
    """Every lens's result for one fixture, plus the verdicts drawn from them."""
    recognition: dict[str, Any]
    blockers: list[str]
    structural: dict[str, Any]
    geometry: dict[str, Any]
    typography: dict[str, Any]
    visual: dict[str, Any]
    form_controls: dict[str, Any]
    classifications: list[dict[str, Any]]
    promotion: dict[str, Any]
    work_counters: dict[str, Any]


def run_lanes(
    importer: Path, observer: Path, source: Path, output: Path,
    timeout_seconds: int, from_source: str, browser_path: Path | None,
    fixture_metadata: dict[str, Any] | None,
) -> LaneOutputs:
    """Import the source through both lanes, render the native IR, verify all of it."""
    browser_dir = output / "browser"
    native_dir = output / "candidate"
    comparison_dir = output / "comparison"
    for directory in (browser_dir, native_dir, comparison_dir):
        directory.mkdir(exist_ok=True)

    browser_ir = browser_dir / "design.ir.json"
    native_ir = native_dir / "design.ir.json"
    native_render = native_dir / "render.png"
    native_layout = native_dir / "layout.json"
    native_receipt = native_dir / "render-receipt.json"
    source_args = ["--from", from_source, "--file", str(source)]
    # Both sides must evaluate the same authored source at the same viewport.
    # The old candidate lane appended --offline, which silently substituted the
    # legacy partial parser and made geometry/typography/pixel results unusable
    # as a native capability baseline.
    viewport = (fixture_metadata or {}).get("viewport", {})
    viewport_width = int(viewport.get("width", 1280))
    viewport_height = int(viewport.get("height", 800))
    viewport_args = ["--render-size", f"{viewport_width}x{viewport_height}"]
    interaction_path = (fixture_metadata or {}).get("browser_interactions")
    interaction_args = []
    if interaction_path:
        interaction_file = (source.parent / interaction_path).resolve()
        if not interaction_file.is_file():
            raise LabError(
                f"browser interaction plan does not exist: {interaction_file.name}")
        interaction_args = ["--browser-interactions", str(interaction_file)]
    browser_args = import_lane_args(
        importer, source_args, browser_ir, viewport_args, interaction_args, browser_path,
        native=False)
    native_args = import_lane_args(
        importer, source_args, native_ir, viewport_args, interaction_args, browser_path,
        native=True)
    browser_run = run_command(browser_args, timeout_seconds)
    native_run = run_command(native_args, timeout_seconds)
    browser_command = command_record(browser_run)
    native_command = command_record(native_run)
    write_json(browser_dir / "command.json", browser_command)
    write_json(native_dir / "import-command.json", native_command)
    if browser_run.returncode != 0:
        raise LabError(
            f"browser import failed ({browser_run.returncode}): {browser_run.stderr}")
    if native_run.returncode != 0:
        raise LabError(
            f"native import failed ({native_run.returncode}): {native_run.stderr}")

    browser_doc = json.loads(browser_ir.read_text(encoding="utf-8"))
    native_doc = json.loads(native_ir.read_text(encoding="utf-8"))
    # The lab artifacts are designed to be shareable: retain source identity by
    # basename + hash, never by a machine-local absolute path.
    browser_doc["sourceFile"] = source.name
    native_doc["sourceFile"] = source.name
    write_json(browser_ir, browser_doc)
    write_json(native_ir, native_doc)
    evidence = find_browser_evidence(browser_ir)
    capture_bytes = (evidence / "capture.json").read_bytes()
    capture = json.loads(capture_bytes)
    native_evidence = find_browser_evidence(native_ir)
    native_capture_bytes = (native_evidence / "capture.json").read_bytes()
    native_capture = json.loads(native_capture_bytes)
    source_sha256 = sha256_file(source)
    expected_interaction_plan_sha256 = None
    if interaction_path:
        expected_interaction_plan_sha256 = interaction_plan_sha256(interaction_file)
    verify_capture_binding(
        capture, native_capture, source_sha256,
        expected_interaction_plan_sha256=expected_interaction_plan_sha256,
    )
    verify_capture_image_binding(capture, evidence / "browser.png")
    verify_capture_image_binding(native_capture, native_evidence / "browser.png")
    browser_snapshot_binding, browser_snapshot_bytes = verify_capture_snapshot_binding(capture, evidence)
    native_snapshot_binding, native_snapshot_bytes = verify_capture_snapshot_binding(native_capture, native_evidence)
    capture_width, capture_height = capture_logical_size(capture)
    authored_x, authored_y = authenticated_authored_origin(capture, native_capture)
    page_background = document_canvas_background(
        json.loads(browser_snapshot_bytes), capture)
    if page_background["status"] == "resolved" and panel_covers_page(capture):
        # A panel that is the whole page owns its canvas: the importer stamps
        # the document colour on its root. Supplying it here would paint a
        # backdrop the import may have dropped.
        page_background = {"status": "importer-owned", "color": None,
                           "reason": "the authored frame covers the page; the "
                                     "importer's root carries the document canvas",
                           "resolved": page_background}
    render_args = [
        str(observer), "--input", str(native_ir),
        "--render", str(native_render), "--layout", str(native_layout),
        "--width", str(capture_width), "--height", str(capture_height),
        "--scale", str(capture["reference"]["device_scale_factor"]),
        "--backend", "skia", "--render-receipt", str(native_receipt),
        "--origin", f"{authored_x},{authored_y}",
    ]
    if page_background["status"] == "resolved":
        render_args.extend(["--page-background", page_background["color"]])
    render_run = run_command(render_args, timeout_seconds)
    write_json(native_dir / "render-command.json", command_record(render_run))
    if render_run.returncode != 0:
        raise LabError(
            f"native render failed ({render_run.returncode}): {render_run.stderr}")
    if not native_receipt.exists():
        raise LabError("native Skia render did not emit a render receipt")
    render_receipt = json.loads(native_receipt.read_text(encoding="utf-8"))
    if (render_receipt.get("schema") != "pulp-design-ir-render-receipt-v1"
            or render_receipt.get("backend_requested") != "skia"
            or render_receipt.get("backend_used") != "skia"
            or render_receipt.get("ok") is not True):
        raise LabError("native render receipt did not prove a successful Skia render")
    # The receipt records how the observer placed the panel; it must be the
    # placement this comparison asked for, or the render was registered by
    # something other than the authenticated authored frame.
    placement = render_receipt.get("placement")
    expected_background = (page_background["color"]
                           if page_background["status"] == "resolved" else "")
    if (not isinstance(placement, dict)
            or not math.isclose(float(placement.get("origin_x", math.nan)), authored_x,
                                abs_tol=1e-4)
            or not math.isclose(float(placement.get("origin_y", math.nan)), authored_y,
                                abs_tol=1e-4)
            or placement.get("page_background") != expected_background):
        raise LabError("native render receipt does not record the requested placement")
    render_command = command_record(render_run)

    try:
        from PIL import Image
        with Image.open(native_render) as image:
            actual_render_size = image.size
    except (ImportError, OSError) as exc:
        raise LabError("native render could not be opened for viewport/DPR audit") from exc
    viewport_dpr = viewport_dpr_audit(
        capture, native_capture, (viewport_width, viewport_height), actual_render_size)
    if not viewport_dpr["ok"]:
        raise LabError("viewport/DPR audit failed: " + json.dumps(viewport_dpr["checks"], sort_keys=True))
    return LaneOutputs(
        browser_dir=browser_dir, native_dir=native_dir, comparison_dir=comparison_dir,
        browser_ir=browser_ir, native_ir=native_ir, native_render=native_render,
        native_layout=native_layout, native_receipt=native_receipt,
        browser_run=browser_run, native_run=native_run, render_run=render_run,
        browser_command=browser_command, native_command=native_command,
        render_command=render_command,
        browser_doc=browser_doc, native_doc=native_doc,
        evidence=evidence, native_evidence=native_evidence,
        capture=capture, native_capture=native_capture,
        capture_bytes=capture_bytes, native_capture_bytes=native_capture_bytes,
        browser_snapshot_binding=browser_snapshot_binding,
        native_snapshot_binding=native_snapshot_binding,
        browser_snapshot_bytes=browser_snapshot_bytes,
        native_snapshot_bytes=native_snapshot_bytes,
        render_receipt=render_receipt, source_sha256=source_sha256,
        capture_size=(capture_width, capture_height), viewport_dpr=viewport_dpr,
        placement_origin=(authored_x, authored_y), page_background=page_background,
    )


def retain_evidence(lanes: LaneOutputs) -> None:
    """Copy the verified capture evidence beside each lane's outputs."""
    for name in (
        ".pulp-browser-capture-v1", "browser.png",
        "semantic-report.json", "tokens.json",
        "interaction-report.json", "platform-fonts.json",
    ):
        source_evidence = lanes.evidence / name
        if source_evidence.exists():
            shutil.copy2(source_evidence, lanes.browser_dir / name)
    # Parse and retain the exact buffer verified above, without reopening a
    # sidecar that could have changed after verification.
    (lanes.browser_dir / "dom-snapshot.json").write_bytes(lanes.browser_snapshot_bytes)
    (lanes.browser_dir / "capture.json").write_bytes(lanes.capture_bytes)
    (lanes.native_dir / "dom-snapshot.json").write_bytes(lanes.native_snapshot_bytes)
    (lanes.native_dir / "capture.json").write_bytes(lanes.native_capture_bytes)


def stage_provenance(lanes: LaneOutputs) -> dict[str, Any]:
    """Keep the design-time browser assist apart from the browser-free runtime.

    The native comparison lane is browser-assisted during design-time import:
    Chromium evaluates the authored HTML and the native lowering consumes that
    capture. The runtime claim is narrower and testable: the observer receives
    DesignIR and renders it through Skia without a browser argument or browser
    capture process. Keeping the two claims separate means the native import
    timing is never misreported as browser-free runtime startup.
    """
    render_argv_lower = [str(value).lower() for value in lanes.render_command["argv"]]
    browser_runtime_tokens = ("chrome", "chromium", "webkit", "browser")
    browser_runtime_token_hits = [
        token for token in render_argv_lower
        if any(marker in token for marker in browser_runtime_tokens)
    ]
    native_provenance = lanes.native_capture.get("provenance", {})
    return {
        "schema": "pulp-importer-differential-stage-provenance-v1",
        "design_time": {
            "native_lane_browser_assisted": (
                native_provenance.get("capture_method") == "chromium-cdp"),
            "capture_method": native_provenance.get("capture_method"),
            "browser_build_hash": native_provenance.get("browser", {}).get("build_hash"),
            "scope": "authored-source evaluation and DesignIR lowering",
        },
        "native_runtime": {
            "browser_required": bool(browser_runtime_token_hits),
            "browser_argument_tokens": browser_runtime_token_hits,
            "renderer_executable": lanes.render_command["executable"],
            "backend_requested": lanes.render_receipt.get("backend_requested"),
            "backend_used": lanes.render_receipt.get("backend_used"),
            "skia_receipt_ok": lanes.render_receipt.get("ok") is True,
            "scope": "DesignIR observation and native readback render",
            "evidence": [
                "candidate/render-command.json",
                "candidate/render-receipt.json",
            ],
        },
    }


def structural_report(
    comparison: dict[str, Any], browser_flat: list[BrowserNode],
    native_flat: list[NativeNode],
) -> dict[str, Any]:
    matches = comparison["matches"]
    ignored_native = comparison["ignored_native"]
    parentage_errors = comparison["parentage_errors"]
    return {
        "score": round(comparison["score"], 6),
        "browser_node_count": len(browser_flat),
        "candidate_node_count": len(native_flat),
        "matched_count": len(matches),
        "structurally_valid_match_count": comparison["structurally_valid_matches"],
        "incorrect_parentage_count": len(parentage_errors),
        "incorrect_parentage": parentage_errors,
        "missing_count": len(comparison["missing"]),
        "extra_count": len(comparison["extra"]),
        "ignored_representation_node_count": len(ignored_native),
        "ignored_representation_nodes": [
            {"anchor": node.anchor, "type": node.node_type, "name": node.name,
             "text": node.text}
            for node in ignored_native
        ],
        "missing": [
            {"index": node.index, "tag": node.tag, "text": node.text}
            for node in comparison["missing"]
        ],
        "extra": [
            {"anchor": node.anchor, "type": node.node_type, "name": node.name,
             "text": node.text}
            for node in comparison["extra"]
        ],
        "matches": [
            {
                "browser_node": browser.index,
                "browser_tag": browser.tag,
                "native_anchor": native.anchor,
                "native_type": native.node_type,
                "match_score": round(score, 3),
            }
            for browser, native, score in matches
        ],
    }


def score_lanes(lanes: LaneOutputs, source: Path) -> Scores:
    """Run every lens over verified lane outputs and draw the verdicts."""
    capture = lanes.capture
    native_capture = lanes.native_capture
    recognition = source_recognition(source.read_text(encoding="utf-8", errors="replace"))
    browser_snapshot = DomSnapshot(json.loads(lanes.browser_snapshot_bytes))
    native_layout_doc = json.loads(lanes.native_layout.read_text(encoding="utf-8"))
    browser_flat_raw = browser_nodes(browser_snapshot)
    # The observer laid the panel out at the placement origin, so its layout
    # is page-space like Chromium's. The registered comparison moves both
    # sides back into the panel's own frame; page-space stays a diagnostic.
    native_flat_page = native_nodes(lanes.native_doc, native_layout_doc)
    native_flat = native_nodes_in_panel_space(native_flat_page, lanes.placement_origin)
    browser_flat, geometry_registration = register_browser_nodes_to_native_frame(
        browser_flat_raw, capture, native_capture)
    comparison = structural_comparison(browser_flat, native_flat)
    matches = comparison["matches"]
    structural = structural_report(comparison, browser_flat, native_flat)
    # Registered panel geometry is gated together with a frame placement
    # proof, so registration cannot hide a misplaced panel. Page-space
    # geometry stays reported as an ungated diagnostic.
    raw_comparison = structural_comparison(browser_flat_raw, native_flat_page)
    frame_placement = frame_placement_check(
        browser_snapshot, lanes.native_doc, native_layout_doc, matches, capture)
    placed_x, placed_y = lanes.placement_origin
    placement = {
        "x": round(placed_x, 6), "y": round(placed_y, 6),
        "source": "authenticated-authored-frame",
        "applied_to": "native-render-and-layout",
        # Capture snaps box paint edges in panel-local CSS px. Moved by a
        # fractional origin they no longer equal Chromium's page-space snap,
        # so edge residuals on such a panel are partly registration.
        "integral_origin": float(placed_x).is_integer() and float(placed_y).is_integer(),
    }
    geometry = geometry_metrics_with_registration(
        raw_comparison["matches"], matches, geometry_registration, frame_placement,
        reference_nodes=browser_flat, placement=placement)
    typography = typography_metrics(matches)
    authored_frame = capture.get("reference", {}).get("authored_frame")
    native_authored_frame = native_capture.get("reference", {}).get("authored_frame")
    if authored_frame != native_authored_frame:
        raise LabError("browser/native authored-frame metadata disagrees for visual comparison")
    native_frame_registration = verify_native_authored_frame(
        lanes.native_doc, native_layout_doc, authored_frame,
        placement_origin=lanes.placement_origin)
    scale = capture["reference"]["device_scale_factor"]
    capture_width, capture_height = lanes.capture_size
    visual = visual_metrics(
        lanes.browser_dir / "browser.png",
        lanes.native_render,
        lanes.comparison_dir / "overlays",
        (round(capture_width * scale), round(capture_height * scale)),
        authored_frame,
        scale,
        lanes.placement_origin,
        lanes.page_background.get("status"),
    )
    visual["registration"]["native_layout"] = native_frame_registration
    if visual.get("registered_visual"):
        visual["registered_visual"]["registration"]["native_layout"] = native_frame_registration
    blockers = unsupported_features(recognition)
    form_control_report = form_control_residual(
        browser_flat_raw, lanes.browser_dir / "browser.png", lanes.native_render, visual,
        browser_snapshot)
    classifications = classify_gaps(
        recognition, structural, geometry, typography, visual,
        form_control_report)
    work_counters = work_counter_gate(lanes.render_receipt)
    recommendation = promotion_recommendation(
        blockers, structural, geometry, typography, visual, work_counters,
        support_boundaries=form_control_report["support_boundaries"])
    return Scores(
        recognition=recognition, blockers=blockers, structural=structural,
        geometry=geometry, typography=typography, visual=visual,
        form_controls=form_control_report, classifications=classifications,
        promotion=recommendation, work_counters=work_counters)


def timings_record(lanes: LaneOutputs) -> dict[str, Any]:
    browser_ms = lanes.browser_run.elapsed_ms
    native_ms = lanes.native_run.elapsed_ms
    render_ms = lanes.render_run.elapsed_ms
    return {
        "browser_import_ms": browser_ms,
        "native_import_ms": native_ms,
        "native_render_ms": render_ms,
        "native_total_ms": native_ms + render_ms,
        "browser_import_timing_line": lanes.browser_command["import_timing_line"],
        "native_import_timing_line": lanes.native_command["import_timing_line"],
        "browser_to_native_import_speedup": round(browser_ms / max(1, native_ms), 3),
    }


def integrity_record(
    lanes: LaneOutputs, importer: Path, observer: Path,
) -> dict[str, Any]:
    capture = lanes.capture
    return {
        "schema": "pulp-importer-differential-integrity-v1",
        "source_sha256": lanes.source_sha256,
        "importer_sha256": sha256_file(importer) if importer.is_file() else None,
        "observer_sha256": sha256_file(observer) if observer.is_file() else None,
        "browser_ir_sha256": sha256_file(lanes.browser_ir),
        "native_ir_sha256": sha256_file(lanes.native_ir),
        # Raw hashes preserve chain of custody. Canonical hashes scrub capture
        # clocks so repeated equivalent runs can actually reach determinism.
        "browser_capture_sha256": hashlib.sha256(lanes.capture_bytes).hexdigest(),
        "native_capture_sha256": hashlib.sha256(lanes.native_capture_bytes).hexdigest(),
        "browser_snapshot_binding": lanes.browser_snapshot_binding,
        "native_snapshot_binding": lanes.native_snapshot_binding,
        "browser_ir_canonical_sha256": canonical_json_sha256(lanes.browser_ir),
        "native_ir_canonical_sha256": canonical_json_sha256(lanes.native_ir),
        "browser_capture_canonical_sha256": canonical_json_sha256(lanes.capture_bytes),
        "native_capture_canonical_sha256": canonical_json_sha256(lanes.native_capture_bytes),
        # The snapshot's own content, which the canonical capture digests
        # cannot see: only per-session CDP identifiers are normalised.
        "browser_snapshot_semantic_sha256": snapshot_semantic_sha256(
            lanes.browser_snapshot_bytes),
        "native_snapshot_semantic_sha256": snapshot_semantic_sha256(
            lanes.native_snapshot_bytes),
        "browser_screenshot_sha256": sha256_file(lanes.browser_dir / "browser.png"),
        "native_render_sha256": sha256_file(lanes.native_render),
        "native_layout_sha256": sha256_file(lanes.native_layout),
        "native_render_receipt_sha256": sha256_file(lanes.native_receipt),
        "stage_provenance": stage_provenance(lanes),
        "platform_fonts_sha256": (
            sha256_file(lanes.evidence / "platform-fonts.json")
            if (lanes.evidence / "platform-fonts.json").is_file() else None),
        "interaction_plan_sha256": (
            capture.get("provenance", {}).get("interactions", {}).get("plan_sha256")
            or "none"),
        "device_scale_factor": capture.get("provenance", {}).get("viewport", {}).get(
            "device_scale_factor", capture.get("reference", {}).get("device_scale_factor")),
        "browser_build_hash": capture.get("provenance", {}).get("browser", {}).get("build_hash"),
        "source_bound": True,
        "viewport_bound": True,
        "browser_bound": True,
        "backend_bound": lanes.render_receipt.get("backend_used") == "skia",
        "offline_parser": False,
        "eligibility": "diagnostic-only-until-three-run-digests-and-counters-agree",
    }


def observability_record(native_import_ms: int, cache_state: str) -> dict[str, Any]:
    """Presentation timings, kept fail-closed.

    The importer and observer run headlessly and the observer emits a
    readback; neither is proof of a real GPU present or native interaction
    event.
    """
    return {
        "schema": OBSERVABILITY_SCHEMA,
        "ttfp": {
            "value_ms": None,
            "status": "unverified",
            "definition": "time to first present",
            "evidence": "no native present timestamp is emitted by this lab",
        },
        "ttni": {
            "value_ms": None,
            "status": "unverified",
            "definition": "time to native interactive",
            "evidence": "no interaction-ready event is emitted by this lab",
        },
        "ttni_proxy": {
            "value_ms": native_import_ms,
            "status": "measured",
            "definition": "time to native import process completion",
            "evidence": "import process completion is a proxy, not interaction readiness",
        },
        "ifnf": {
            "value_ms": None,
            "status": "readback-only",
            "definition": "import to first validated native frame",
            "evidence": "CPU/readback render completion cannot establish first native GPU frame",
        },
        "cache_state": {
            "identity": cache_state,
            "status": "declared" if cache_state in ("cold", "warm") else "unverified",
            "source": "caller-declared; never inferred from elapsed time",
            "scope": "importer-differential-lab",
            "dimensions": {
                "source": "not-measured",
                "browser": "not-measured",
                "pulp_import": "not-measured",
                "asset_decode": "not-measured",
                "graphite_resource": "not-applicable",
                "pipeline": "not-applicable",
                "dawn_device": "not-applicable",
                "readback": "not-measured",
            },
        },
    }


def compare_one(
    importer: Path,
    observer: Path,
    source: Path,
    output: Path,
    timeout_seconds: int,
    from_source: str,
    browser_path: Path | None,
    fixture_metadata: dict[str, Any] | None = None,
    cache_state: str = "unknown",
    timing_budgets_ms: dict[str, int] | None = None,
) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    source_copy_dir = output / "source"
    source_copy_dir.mkdir(exist_ok=True)
    shutil.copy2(source, source_copy_dir / source.name)
    lanes = run_lanes(importer, observer, source, output, timeout_seconds,
                      from_source, browser_path, fixture_metadata)
    retain_evidence(lanes)
    scores = score_lanes(lanes, source)
    timings = timings_record(lanes)
    capture = lanes.capture
    native_capture = lanes.native_capture
    report = {
        "schema": REPORT_SCHEMA,
        "version": SCHEMA_VERSION,
        "fixture": fixture_metadata,
        "source": {
            "path": source.name,
            "sha256": sha256_file(source),
        },
        "oracle": {
            "authority": "chromium",
            "capture_method": lanes.browser_doc.get("capture_method"),
            "browser": capture["provenance"]["browser"],
            "viewport": capture["provenance"]["viewport"],
        },
        "candidate": {
            "authority": "advisory",
            "capture_method": lanes.native_doc.get("capture_method"),
            "source_adapter": lanes.native_doc.get("source_adapter"),
            "comparison_lane": "browser-native-panel-lowering",
            "offline_parser": False,
            "provenance": {
                "source_sha256": lanes.source_sha256,
                "browser": native_capture["provenance"]["browser"],
                "viewport": native_capture["provenance"]["viewport"],
                "capture_sha256": sha256_file(lanes.native_evidence / "capture.json"),
            },
        },
        "source_recognition": scores.recognition,
        "unsupported_dynamic_features": scores.blockers,
        "timings": timings,
        "timing_budget": timing_budget_result(timings, timing_budgets_ms),
        "integrity": integrity_record(lanes, importer, observer),
        "observability": observability_record(lanes.native_run.elapsed_ms, cache_state),
        "receipt_paths": {
            "fixture_report": "comparison/report.json",
            "browser_capture": "browser/capture.json",
            "native_readback": "candidate/render.png",
            "native_render_receipt": "candidate/render-receipt.json",
        },
        "comparison": {
            "structural": scores.structural,
            "geometry": scores.geometry,
            "typography": scores.typography,
            "visual": scores.visual,
            "viewport_dpr_audit": lanes.viewport_dpr,
            "form_controls": scores.form_controls,
            "page_background": lanes.page_background,
            "work_counters": scores.work_counters,
        },
        "classifications": scores.classifications,
        "promotion": scores.promotion,
        "canonical_output_modified": False,
    }
    comparison_dir = lanes.comparison_dir
    write_json(comparison_dir / "report.json", report)
    write_json(comparison_dir / "structural-diff.json", scores.structural)
    write_json(comparison_dir / "classifications.json", scores.classifications)
    (comparison_dir / "summary.md").write_text(format_summary(report), encoding="utf-8")
    return report


HOLDOUT_DIRECTORY = "holdout"


def source_files(root: Path, include_holdout: bool = False) -> list[Path]:
    # JSX/TSX requires a build/staging contract before it becomes runnable
    # browser input. The first slice compares only self-contained HTML.
    # Held-out fixtures under a `holdout/` directory score only when asked
    # for, so iterating on a corpus root cannot tune against them.
    suffixes = {".html", ".htm"}
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in suffixes
        and (include_holdout
             or HOLDOUT_DIRECTORY not in path.relative_to(root).parts[:-1]))
