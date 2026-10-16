#!/usr/bin/env python3
"""Chromium-oracle comparison core; it never changes canonical import output."""

from __future__ import annotations

from collections import Counter
import hashlib
import json
import math
import re
import shutil
import statistics
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .recognition import source_recognition, unsupported_features
from .reports import format_summary

SCHEMA_VERSION = 1
REPORT_SCHEMA = "pulp-importer-differential-report-v1"
CORPUS_SCHEMA = "pulp-importer-differential-corpus-v1"
OBSERVABILITY_SCHEMA = "pulp-importer-observability-v1"
TIMING_BUDGET_KEYS = (
    "browser_import_ms", "native_import_ms", "native_render_ms", "native_total_ms",
)

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

PROMOTION_THRESHOLDS = {
    "structural": 0.99,
    "geometry": 0.98,
    "typography": 0.98,
    "visual": 0.995,
}

ROOT_CAUSE_ORDER = [
    "javascript-evaluation", "conditional-rendering", "async-initialization",
    "canvas", "webgl", "pseudo-element", "css-variable", "responsive-style",
    "flex-layout", "grid-layout", "intrinsic-sizing", "absolute-positioning",
    "font-resolution", "font-metrics", "text-wrapping", "svg",
    "image-loading", "transform", "animation", "portal",
    "unsupported-browser-api", "source-recognition", "unknown",
]


class LabError(RuntimeError):
    pass


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


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1,
                               math.ceil(len(ordered) * fraction) - 1))]


@dataclass
class CommandResult:
    argv: list[str]
    elapsed_ms: int
    returncode: int
    stdout: str
    stderr: str


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


def canonical_json_sha256(source: Path | bytes) -> str:
    """Hash JSON evidence after removing documented per-run envelope fields.

    Capture and IR receipts retain elapsed/import timestamps for diagnostics.
    Those fields must not make an otherwise identical campaign appear
    nondeterministic. Browser-capture envelopes also omit their exact snapshot
    binding here: CDP frame IDs vary per capture. Each raw binding is verified
    separately; this digest does not establish semantic DOM determinism.
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
            return {
                key: scrub(item, (*path, key))
                for key, item in value.items()
                if key not in volatile_keys
                and not (document_entry and key == "snapshot_sha256")
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
        return CommandResult(
            argv, elapsed, 124, exc.stdout or "", exc.stderr or "command timed out")
    elapsed = int((time.monotonic() - start) * 1000)
    return CommandResult(
        argv, elapsed, result.returncode, result.stdout, result.stderr)


def command_record(result: CommandResult) -> dict[str, Any]:
    return {
        "elapsed_ms": result.elapsed_ms,
        "exit_code": result.returncode,
        "executable": Path(result.argv[0]).name if result.argv else "",
    }


def decode_string(strings: list[str], index: Any) -> str:
    if isinstance(index, int) and 0 <= index < len(strings):
        return strings[index]
    return ""


def browser_nodes(snapshot: dict[str, Any]) -> list[BrowserNode]:
    strings = snapshot.get("strings", [])
    documents = snapshot.get("documents", [])
    if not documents:
        return []
    document = documents[0]
    raw_nodes = document.get("nodes", {})
    layout = document.get("layout", {})
    style_names = snapshot.get("computedStyleNames") or LEGACY_COMPUTED_STYLES
    layout_by_node = {
        node_index: layout_index
        for layout_index, node_index in enumerate(layout.get("nodeIndex", []))
    }
    parents = raw_nodes.get("parentIndex", [])
    types = raw_nodes.get("nodeType", [])
    names = raw_nodes.get("nodeName", [])
    values = raw_nodes.get("nodeValue", [])
    backend_node_ids = raw_nodes.get("backendNodeId", [])
    raw_attributes = raw_nodes.get("attributes", [])
    children: dict[int, list[int]] = {}
    for index, parent in enumerate(parents):
        children.setdefault(parent, []).append(index)

    def descendant_text(index: int) -> str:
        parts: list[str] = []
        stack = list(children.get(index, []))
        while stack:
            child = stack.pop()
            if child < len(types) and types[child] == 3:
                value = decode_string(strings, values[child])
                if value.strip():
                    parts.append(value.strip())
            stack.extend(children.get(child, []))
        return " ".join(reversed(parts)).strip()

    result: list[BrowserNode] = []
    for index, node_type in enumerate(types):
        if node_type != 1:
            continue
        tag = decode_string(strings, names[index]).lower()
        if tag in {"html", "head", "body", "script", "style", "link", "meta"}:
            continue
        attrs: dict[str, str] = {}
        pairs = raw_attributes[index] if index < len(raw_attributes) else []
        for offset in range(0, len(pairs) - 1, 2):
            attrs[decode_string(strings, pairs[offset])] = decode_string(
                strings, pairs[offset + 1])
        layout_index = layout_by_node.get(index)
        bounds = None
        styles: dict[str, str] = {}
        if layout_index is not None:
            raw_bounds = layout.get("bounds", [])
            if layout_index < len(raw_bounds) and len(raw_bounds[layout_index]) == 4:
                bounds = tuple(float(v) for v in raw_bounds[layout_index])
            raw_styles = layout.get("styles", [])
            if layout_index < len(raw_styles):
                values_for_style = raw_styles[layout_index]
                # zip() truncates in silence, which is precisely how a stale
                # property list turns into shifted-by-one values that read as
                # real style data. A row that does not match the declared
                # request order is a decoding failure, not a short row.
                if len(values_for_style) != len(style_names):
                    raise LabError(
                        f"snapshot style row has {len(values_for_style)} "
                        f"values but declares {len(style_names)} property "
                        "names; the capture and this reader have drifted")
                styles = {
                    name: decode_string(strings, value)
                    for name, value in zip(style_names, values_for_style)
                }
        if bounds and (bounds[2] <= 0 or bounds[3] <= 0):
            continue
        if styles.get("display") == "none" or styles.get("visibility") == "hidden":
            continue
        result.append(BrowserNode(
            index=index,
            tag=tag,
            parent=parents[index] if index < len(parents) else -1,
            text=descendant_text(index),
            attributes=attrs,
            bounds=bounds,
            styles=styles,
            backend_node_id=(
                int(backend_node_ids[index])
                if index < len(backend_node_ids)
                and isinstance(backend_node_ids[index], int)
                and backend_node_ids[index] > 0
                else None),
        ))
    return result


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

    def visit(node: dict[str, Any], parent: str | None) -> None:
        anchor = str(node.get("stable_anchor_id") or "")
        node_type = str(node.get("type") or "unknown")
        text = str(node.get("content") or node.get("text") or "").strip()
        result.append(NativeNode(
            anchor=anchor,
            node_type=node_type,
            name=str(node.get("name") or ""),
            text=text,
            parent_anchor=parent,
            style=node.get("style") if isinstance(node.get("style"), dict) else {},
            bounds=layout_by_anchor.get(anchor),
            backend_node_id=(
                int(node.get("attributes", {}).get("backend_node_id"))
                if isinstance(node.get("attributes"), dict)
                and str(node.get("attributes", {}).get("backend_node_id", "")).isdigit()
                else None),
        ))
        for child in node.get("children", []):
            if isinstance(child, dict):
                visit(child, anchor or parent)

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
    wrappers, and explicit text children. Counting those implementation nodes
    as unmatched browser elements makes an exact element join look like a
    structural failure. Keep them in the report as ignored representation
    nodes, but exclude them from precision/recall.
    """
    ignored_names = {"html", "body"}
    comparable: list[NativeNode] = []
    ignored: list[NativeNode] = []
    for node in native:
        if (node.node_type.lower() == "text"
                or node.anchor == "browser:root"
                or node.name.lower() in ignored_names):
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
        if not browser.bounds or not native.bounds:
            continue
        labels = ("x", "y", "width", "height")
        signed_delta = [candidate - reference
                        for reference, candidate in zip(browser.bounds, native.bounds)]
        component_errors = [
            relative_error(reference, candidate)
            for reference, candidate in zip(browser.bounds, native.bounds)
        ]
        mean_error = statistics.fmean(component_errors)
        errors.append(mean_error)
        rows.append({
            "browser_node": browser.index,
            "native_anchor": native.anchor,
            "browser": dict(zip(labels, browser.bounds)),
            "candidate": dict(zip(labels, native.bounds)),
            # Keep the direction visible. An absolute score can hide a
            # systematic offset by collapsing +N and -N into the same value.
            "signed_delta": dict(zip(labels, [round(v, 6) for v in signed_delta])),
            "max_abs_delta": round(max(abs(v) for v in signed_delta), 6),
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


def geometry_metrics_with_registration(
    raw_matches: list[tuple[BrowserNode, NativeNode, float]],
    registered_matches: list[tuple[BrowserNode, NativeNode, float]],
    registration: dict[str, float],
) -> dict[str, Any]:
    """Return canonical page-space geometry plus an explicit diagnostic view.

    Promotion and aggregate scores must remain based on the raw page-space
    comparison.  The registered view explains an authenticated panel-local
    frame contract without allowing that registration to hide a placement
    mismatch from callers consuming the canonical score.
    """
    raw_geometry = geometry_metrics(raw_matches)
    raw_geometry["registration"] = {
        "x": 0.0, "y": 0.0, "source": "page-space",
    }
    raw_geometry["registered_geometry"] = geometry_metrics(registered_matches)
    raw_geometry["registration_applied"] = registration
    return raw_geometry


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
    # The observer is explicitly invoked with the full document extent but
    # the imported native root is panel-local at origin (the importer records
    # the authored frame for consumers, it does not place the panel in the
    # observer canvas).  Register page-space DOM bounds into that origin.
    dx = -frame_x
    dy = -frame_y
    if abs(dx) < 1e-9 and abs(dy) < 1e-9:
        return nodes, {"x": 0.0, "y": 0.0, "source": "authenticated-authored-frame"}
    registered: list[BrowserNode] = []
    for node in nodes:
        bounds = node.bounds
        if bounds is not None:
            bounds = (bounds[0] + dx, bounds[1] + dy, bounds[2], bounds[3])
        registered.append(BrowserNode(
            index=node.index, tag=node.tag, parent=node.parent, text=node.text,
            attributes=node.attributes, bounds=bounds, styles=node.styles,
            backend_node_id=node.backend_node_id))
    return registered, {"x": round(dx, 6), "y": round(dy, 6),
                       "source": "authenticated-authored-frame"}


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


def visual_metrics(
    reference_path: Path,
    candidate_path: Path,
    artifacts_dir: Path,
    expected_size: tuple[int, int] | None = None,
) -> dict[str, Any]:
    try:
        from PIL import Image, ImageChops
    except ImportError as exc:
        raise LabError("Pillow is required for visual comparison") from exc
    reference = Image.open(reference_path).convert("RGBA")
    candidate = Image.open(candidate_path).convert("RGBA")
    if expected_size and reference.size != expected_size:
        raise LabError(
            "browser screenshot dimensions do not match the captured DPR: "
            f"expected {expected_size}, got {reference.size}")
    if expected_size and candidate.size != expected_size:
        raise LabError(
            "candidate render dimensions do not match the captured DPR: "
            f"expected {expected_size}, got {candidate.size}")
    if reference.size != candidate.size:
        raise LabError(
            "browser and candidate render dimensions differ: "
            f"{reference.size} vs {candidate.size}")
    logical_size = candidate.size
    ref_pixels = list(reference.get_flattened_data())
    candidate_pixels = list(candidate.get_flattened_data())
    total_distance = 0.0
    alpha_distance = 0.0
    foreground_distance = 0.0
    foreground_samples = 0
    differing = 0
    threshold = 2
    reference_background = Counter(ref_pixels).most_common(1)[0][0]
    candidate_background = Counter(candidate_pixels).most_common(1)[0][0]
    for ref_pixel, candidate_pixel in zip(ref_pixels, candidate_pixels):
        delta = sum(abs(a - b) for a, b in zip(ref_pixel[:3], candidate_pixel[:3]))
        alpha_delta = abs(ref_pixel[3] - candidate_pixel[3])
        alpha_distance += alpha_delta
        total_distance += delta
        if delta > threshold or alpha_delta > threshold:
            differing += 1
        ref_foreground = sum(
            abs(a - b) for a, b in zip(ref_pixel[:3], reference_background[:3])) > 18
        candidate_foreground = sum(
            abs(a - b) for a, b in zip(
                candidate_pixel[:3], candidate_background[:3])) > 18
        if ref_foreground or candidate_foreground:
            foreground_distance += delta
            foreground_samples += 1
    samples = max(1, len(ref_pixels))
    mean_channel_error = total_distance / (samples * 3.0)
    mean_alpha_error = alpha_distance / samples
    combined_mean_error = (total_distance + alpha_distance) / (samples * 4.0)
    global_similarity = max(0.0, 1.0 - combined_mean_error / 255.0)
    foreground_mean_error = (
        foreground_distance / (foreground_samples * 3.0)
        if foreground_samples else mean_channel_error)
    foreground_similarity = max(0.0, 1.0 - foreground_mean_error / 255.0)
    similarity = min(global_similarity, foreground_similarity)
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    side_by_side = Image.new(
        "RGBA", (logical_size[0] * 2, logical_size[1]))
    side_by_side.paste(reference, (0, 0))
    side_by_side.paste(candidate, (logical_size[0], 0))
    side_path = artifacts_dir / "side-by-side.png"
    side_by_side.save(side_path)
    diff = ImageChops.difference(reference, candidate).convert("RGB")
    heatmap = diff.point(lambda value: min(255, value * 4))
    heatmap_path = artifacts_dir / "visual-diff.png"
    heatmap.save(heatmap_path)
    overlay = Image.blend(reference, candidate, 0.5)
    overlay_path = artifacts_dir / "overlay.png"
    overlay.save(overlay_path)
    return {
        "score": round(similarity, 6),
        "global_score": round(global_similarity, 6),
        "foreground_score": round(foreground_similarity, 6),
        "mean_channel_error": round(mean_channel_error, 6),
        "mean_alpha_error": round(mean_alpha_error, 6),
        "combined_mean_channel_error": round(combined_mean_error, 6),
        "foreground_mean_channel_error": round(foreground_mean_error, 6),
        "foreground_pixel_count": foreground_samples,
        "differing_pixel_count": differing,
        "differing_pixel_fraction": round(differing / samples, 6),
        "width": logical_size[0],
        "height": logical_size[1],
        "artifacts": {
            "side_by_side": "overlays/side-by-side.png",
            "heatmap": "overlays/visual-diff.png",
            "overlay": "overlays/overlay.png",
        },
    }


def classify_gaps(
    recognition: dict[str, Any],
    structural: dict[str, Any],
    geometry: dict[str, Any],
    typography: dict[str, Any],
    visual: dict[str, Any],
) -> list[dict[str, Any]]:
    features = recognition["features"]
    facts: dict[str, tuple[str, float, str]] = {}
    mappings = {
        "canvas": ("canvas", 1.0),
        "webgl": ("webgl", 1.0),
        "pseudo_elements": ("pseudo-element", 0.95),
        "css_variables": ("css-variable", 0.9),
        "media_queries": ("responsive-style", 0.9),
        "flex_layout": ("flex-layout", 0.85),
        "grid_layout": ("grid-layout", 0.85),
        "absolute_positioning": ("absolute-positioning", 0.85),
        "custom_fonts": ("font-resolution", 0.8),
        "svg": ("svg", 0.8),
        "images": ("image-loading", 0.8),
        "animation": ("animation", 0.95),
        "portals": ("portal", 0.95),
        "async_initialization": ("async-initialization", 0.95),
        "conditional_rendering": ("conditional-rendering", 0.9),
    }
    for feature, (cause, confidence) in mappings.items():
        if features.get(feature):
            facts[cause] = (
                f"source recognition observed {feature.replace('_', ' ')}",
                confidence,
                "source-recognition",
            )
    if recognition["framework"] in {"react", "vue", "svelte"}:
        facts["javascript-evaluation"] = (
            f"{recognition['framework']} source requires evaluated component state",
            0.98,
            "runtime-importer",
        )
    if structural["score"] < PROMOTION_THRESHOLDS["structural"]:
        facts.setdefault("source-recognition", (
            f"{structural['missing_count']} browser nodes were not matched",
            0.75,
            "parser",
        ))
    if geometry["score"] < PROMOTION_THRESHOLDS["geometry"]:
        likely = "flex-layout" if features.get("flex_layout") else (
            "grid-layout" if features.get("grid_layout") else "intrinsic-sizing")
        facts.setdefault(likely, (
            f"geometry score was {geometry['score']:.3f}",
            0.65,
            "layout",
        ))
    if typography["score"] < PROMOTION_THRESHOLDS["typography"]:
        facts.setdefault("font-metrics", (
            f"typography score was {typography['score']:.3f}",
            0.6,
            "text",
        ))
    if visual["score"] < PROMOTION_THRESHOLDS["visual"] and not facts:
        facts["unknown"] = (
            f"visual score was {visual['score']:.3f}",
            0.3,
            "unknown",
        )
    result = []
    for cause in ROOT_CAUSE_ORDER:
        if cause not in facts:
            continue
        evidence, confidence, subsystem = facts[cause]
        result.append({
            "cause": cause,
            "status": "hypothesis",
            "confidence": confidence,
            "supporting_evidence": evidence,
            "suggested_subsystem": subsystem,
        })
    return result


def promotion_recommendation(
    unsupported: list[str],
    structural: dict[str, Any],
    geometry: dict[str, Any],
    typography: dict[str, Any],
    visual: dict[str, Any],
) -> dict[str, Any]:
    scores = {
        "structural": structural["score"],
        "geometry": geometry["score"],
        "typography": typography["score"],
        "visual": visual["score"],
    }
    failed = [
        name for name, threshold in PROMOTION_THRESHOLDS.items()
        if scores[name] < threshold
    ]
    threshold_eligible = not unsupported and not failed
    if unsupported:
        classification = "browser-required"
        reasons = [f"unsupported dynamic features: {', '.join(unsupported)}"]
    else:
        classification = "native-with-browser-validation"
        reasons = (
            [f"{name} score below {PROMOTION_THRESHOLDS[name]:.3f}"
             for name in failed]
            if failed else
            ["threshold-eligible, but production promotion is disabled until "
             "a candidate-only classifier passes a frozen holdout corpus"])
    return {
        "classification": classification,
        "advisory_only": True,
        "production_promotion_enabled": False,
        "threshold_eligible": threshold_eligible,
        "thresholds": PROMOTION_THRESHOLDS,
        "reasons": reasons,
    }


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
            return snapshot.read_bytes()
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
    source_copy = source_copy_dir / source.name
    shutil.copy2(source, source_copy)
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
    write_json(browser_dir / "command.json", command_record(browser_run))
    write_json(native_dir / "import-command.json", command_record(native_run))
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
    viewport = capture["provenance"]["viewport"]["resolved"]
    capture_width, capture_height = capture_logical_size(capture)
    render_args = [
        str(observer), "--input", str(native_ir),
        "--render", str(native_render), "--layout", str(native_layout),
        "--width", str(capture_width), "--height", str(capture_height),
        "--scale", str(capture["reference"]["device_scale_factor"]),
        "--backend", "skia", "--render-receipt", str(native_receipt),
    ]
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

    for name in (
        ".pulp-browser-capture-v1", "browser.png",
        "semantic-report.json", "tokens.json",
        "interaction-report.json", "platform-fonts.json",
    ):
        source_evidence = evidence / name
        if source_evidence.exists():
            shutil.copy2(source_evidence, browser_dir / name)
    # Parse and retain the exact buffer verified above, without reopening a
    # sidecar that could have changed after verification.
    (browser_dir / "dom-snapshot.json").write_bytes(browser_snapshot_bytes)
    (browser_dir / "capture.json").write_bytes(capture_bytes)
    (native_dir / "dom-snapshot.json").write_bytes(native_snapshot_bytes)
    (native_dir / "capture.json").write_bytes(native_capture_bytes)
    recognition = source_recognition(source.read_text(encoding="utf-8", errors="replace"))
    browser_snapshot = json.loads(browser_snapshot_bytes)
    native_layout_doc = json.loads(native_layout.read_text(encoding="utf-8"))
    browser_flat_raw = browser_nodes(browser_snapshot)
    native_flat = native_nodes(native_doc, native_layout_doc)
    browser_flat, geometry_registration = register_browser_nodes_to_native_frame(
        browser_flat_raw, capture, native_capture)
    comparison = structural_comparison(browser_flat, native_flat)
    matches = comparison["matches"]
    missing = comparison["missing"]
    extra = comparison["extra"]
    ignored_native = comparison["ignored_native"]
    parentage_errors = comparison["parentage_errors"]
    structurally_valid_matches = comparison["structurally_valid_matches"]
    structural_score = comparison["score"]
    structural = {
        "score": round(structural_score, 6),
        "browser_node_count": len(browser_flat),
        "candidate_node_count": len(native_flat),
        "matched_count": len(matches),
        "structurally_valid_match_count": structurally_valid_matches,
        "incorrect_parentage_count": len(parentage_errors),
        "incorrect_parentage": parentage_errors,
        "missing_count": len(missing),
        "extra_count": len(extra),
        "ignored_representation_node_count": len(ignored_native),
        "ignored_representation_nodes": [
            {"anchor": node.anchor, "type": node.node_type, "name": node.name,
             "text": node.text}
            for node in ignored_native
        ],
        "missing": [
            {"index": node.index, "tag": node.tag, "text": node.text}
            for node in missing
        ],
        "extra": [
            {"anchor": node.anchor, "type": node.node_type, "name": node.name,
             "text": node.text}
            for node in extra
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
    # Keep page-space geometry canonical. Registered panel-space geometry is a
    # diagnostic view of the intentional panel-local contract and must never
    # silently turn a placement mismatch into a promotion pass.
    raw_comparison = structural_comparison(browser_flat_raw, native_flat)
    geometry = geometry_metrics_with_registration(
        raw_comparison["matches"], matches, geometry_registration)
    typography = typography_metrics(matches)
    visual = visual_metrics(
        browser_dir / "browser.png",
        native_render,
        comparison_dir / "overlays",
        (
            round(capture_width * capture["reference"]["device_scale_factor"]),
            round(capture_height * capture["reference"]["device_scale_factor"]),
        ),
    )
    blockers = unsupported_features(recognition)
    classifications = classify_gaps(
        recognition, structural, geometry, typography, visual)
    recommendation = promotion_recommendation(
        blockers, structural, geometry, typography, visual)
    timings = {
        "browser_import_ms": browser_run.elapsed_ms,
        "native_import_ms": native_run.elapsed_ms,
        "native_render_ms": render_run.elapsed_ms,
        "native_total_ms": native_run.elapsed_ms + render_run.elapsed_ms,
        "browser_to_native_import_speedup": round(
            browser_run.elapsed_ms / max(1, native_run.elapsed_ms), 3),
    }
    timing_budget = timing_budget_result(timings, timing_budgets_ms)
    integrity = {
        "schema": "pulp-importer-differential-integrity-v1",
        "source_sha256": source_sha256,
        "importer_sha256": sha256_file(importer) if importer.is_file() else None,
        "observer_sha256": sha256_file(observer) if observer.is_file() else None,
        "browser_ir_sha256": sha256_file(browser_ir),
        "native_ir_sha256": sha256_file(native_ir),
        # Raw hashes preserve chain of custody. Canonical hashes scrub capture
        # clocks so repeated equivalent runs can actually reach determinism.
        "browser_capture_sha256": hashlib.sha256(capture_bytes).hexdigest(),
        "native_capture_sha256": hashlib.sha256(native_capture_bytes).hexdigest(),
        "browser_snapshot_binding": browser_snapshot_binding,
        "native_snapshot_binding": native_snapshot_binding,
        "browser_ir_canonical_sha256": canonical_json_sha256(browser_ir),
        "native_ir_canonical_sha256": canonical_json_sha256(native_ir),
        "browser_capture_canonical_sha256": canonical_json_sha256(capture_bytes),
        "native_capture_canonical_sha256": canonical_json_sha256(native_capture_bytes),
        "browser_screenshot_sha256": sha256_file(browser_dir / "browser.png"),
        "native_render_sha256": sha256_file(native_render),
        "native_layout_sha256": sha256_file(native_layout),
        "native_render_receipt_sha256": sha256_file(native_receipt),
        "platform_fonts_sha256": (
            sha256_file(evidence / "platform-fonts.json")
            if (evidence / "platform-fonts.json").is_file() else None),
        "interaction_plan_sha256": (
            capture.get("provenance", {}).get("interactions", {}).get("plan_sha256")
            or "none"),
        "device_scale_factor": capture.get("provenance", {}).get("viewport", {}).get(
            "device_scale_factor", capture.get("reference", {}).get("device_scale_factor")),
        "browser_build_hash": capture.get("provenance", {}).get("browser", {}).get("build_hash"),
        "source_bound": True,
        "viewport_bound": True,
        "browser_bound": True,
        "backend_bound": render_receipt.get("backend_used") == "skia",
        "offline_parser": False,
        "eligibility": "diagnostic-only-until-three-run-digests-and-counters-agree",
    }
    # These timings deliberately keep presentation claims fail-closed. The
    # importer and observer run headlessly and the observer emits a readback;
    # neither is proof of a real GPU present or native interaction event.
    observability = {
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
            "value_ms": native_run.elapsed_ms,
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
            "capture_method": browser_doc.get("capture_method"),
            "browser": capture["provenance"]["browser"],
            "viewport": capture["provenance"]["viewport"],
        },
        "candidate": {
            "authority": "advisory",
            "capture_method": native_doc.get("capture_method"),
            "source_adapter": native_doc.get("source_adapter"),
            "comparison_lane": "browser-native-panel-lowering",
            "offline_parser": False,
            "provenance": {
                "source_sha256": source_sha256,
                "browser": native_capture["provenance"]["browser"],
                "viewport": native_capture["provenance"]["viewport"],
                "capture_sha256": sha256_file(native_evidence / "capture.json"),
            },
        },
        "source_recognition": recognition,
        "unsupported_dynamic_features": blockers,
        "timings": timings,
        "timing_budget": timing_budget,
        "integrity": integrity,
        "observability": observability,
        "receipt_paths": {
            "fixture_report": "comparison/report.json",
            "browser_capture": "browser/capture.json",
            "native_readback": "candidate/render.png",
            "native_render_receipt": "candidate/render-receipt.json",
        },
        "comparison": {
            "structural": structural,
            "geometry": geometry,
            "typography": typography,
            "visual": visual,
            "viewport_dpr_audit": viewport_dpr,
        },
        "classifications": classifications,
        "promotion": recommendation,
        "canonical_output_modified": False,
    }
    write_json(comparison_dir / "report.json", report)
    write_json(comparison_dir / "structural-diff.json", structural)
    write_json(comparison_dir / "classifications.json", classifications)
    (comparison_dir / "summary.md").write_text(format_summary(report), encoding="utf-8")
    return report


def source_files(root: Path) -> list[Path]:
    # JSX/TSX requires a build/staging contract before it becomes runnable
    # browser input. The first slice compares only self-contained HTML.
    suffixes = {".html", ".htm"}
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in suffixes)


def aggregate_reports(
    reports: list[dict[str, Any]], failure_count: int = 0
) -> dict[str, Any]:
    classifications = {
        "native-authoritative": 0,
        "native-with-browser-validation": 0,
        "browser-required": 0,
        "unsupported": 0,
    }
    gap_counts: dict[str, dict[str, Any]] = {}
    for report in reports:
        classifications[report["promotion"]["classification"]] += 1
        impact = 1.0 - report["comparison"]["visual"]["score"]
        for finding in report["classifications"]:
            bucket = gap_counts.setdefault(finding["cause"], {
                "cause": finding["cause"],
                "affected": 0,
                "visual_impact_sum": 0.0,
                "confidence_sum": 0.0,
                "suggested_subsystem": finding["suggested_subsystem"],
            })
            bucket["affected"] += 1
            bucket["visual_impact_sum"] += impact
            bucket["confidence_sum"] += finding["confidence"]
    ranked = []
    for bucket in gap_counts.values():
        affected = bucket["affected"]
        ranked.append({
            "cause": bucket["cause"],
            "affected": affected,
            "affected_fraction": round(affected / max(1, len(reports)), 6),
            "mean_visual_impact": round(
                bucket["visual_impact_sum"] / affected, 6),
            "mean_confidence": round(bucket["confidence_sum"] / affected, 6),
            "suggested_subsystem": bucket["suggested_subsystem"],
            "priority_score": round(
                affected * (bucket["visual_impact_sum"] / affected)
                * (bucket["confidence_sum"] / affected), 6),
        })
    ranked.sort(key=lambda row: (-row["priority_score"], row["cause"]))

    def mean(path: tuple[str, ...]) -> float:
        values = []
        for report in reports:
            value: Any = report
            for key in path:
                value = value[key]
            values.append(float(value))
        return round(statistics.fmean(values), 6) if values else 0.0

    total = len(reports) + failure_count
    classifications["unsupported"] = failure_count
    false_promotions = sum(
        1 for report in reports
        if report["promotion"]["classification"] == "native-authoritative"
        and not report["promotion"]["threshold_eligible"])
    budget_reports = [report.get("timing_budget", {}) for report in reports]
    budget_requested = any(
        report.get("status") in ("pass", "fail") for report in budget_reports)
    budget_failed = any(report.get("status") == "fail" for report in budget_reports)
    return {
        "schema": CORPUS_SCHEMA,
        "version": SCHEMA_VERSION,
        "fixture_count": total,
        "completed_fixture_count": len(reports),
        "failed_fixture_count": failure_count,
        "classifications": classifications,
        "shares": {
            key: round(value / max(1, total), 6)
            for key, value in classifications.items()
        },
        "mean_scores": {
            name: mean(("comparison", name, "score"))
            for name in ("structural", "geometry", "typography", "visual")
        },
        "mean_timings_ms": {
            "browser_import": mean(("timings", "browser_import_ms")),
            "native_import": mean(("timings", "native_import_ms")),
            "native_render": mean(("timings", "native_render_ms")),
            "native_total": mean(("timings", "native_total_ms")),
        },
        "mean_import_only_speedup": mean(
            ("timings", "browser_to_native_import_speedup")),
        "false_promotions": false_promotions,
        "false_promotion_status": (
            "not-evaluable-production-promotion-disabled"
            if not any(
                report["promotion"].get("production_promotion_enabled", False)
                for report in reports)
            else "evaluated"),
        "timing_budget": {
            "status": "fail" if budget_failed else
                ("pass" if budget_requested else "not-requested"),
            "budgets_ms": next((report.get("budgets_ms", {}) for report in budget_reports
                                if report.get("budgets_ms")), {}),
            "failed_fixture_count": sum(
                report.get("status") == "fail" for report in budget_reports),
        },
        "observability": {
            "ttfp": {"value_ms": None, "status": "unverified"},
            "ttni": {"p50_ms": None, "p95_ms": None, "status": "unverified"},
            "ttni_proxy": {
                "p50_ms": round(statistics.median([
                    report.get("observability", {}).get("ttni_proxy", {}).get(
                        "value_ms", report["timings"]["native_import_ms"])
                    for report in reports]), 1) if reports else None,
                "p95_ms": percentile([
                    report.get("observability", {}).get("ttni_proxy", {}).get(
                        "value_ms", report["timings"]["native_import_ms"])
                    for report in reports], 0.95) if reports else None,
                "status": "measured" if reports else "unverified",
            },
            "ifnf": {"value_ms": None, "status": "readback-only"},
            "cache_states": sorted({
                report.get("observability", {}).get("cache_state", {}).get(
                    "identity", "unknown") for report in reports}),
        },
        "ranked_gaps": ranked,
    }
