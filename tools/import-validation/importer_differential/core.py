#!/usr/bin/env python3
"""Chromium-oracle comparison core; it never changes canonical import output."""

from __future__ import annotations

from collections import Counter
import hashlib
import json
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

COMPUTED_STYLES = [
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


@dataclass
class NativeNode:
    anchor: str
    node_type: str
    name: str
    text: str
    parent_anchor: str | None
    style: dict[str, Any]
    bounds: tuple[float, float, float, float] | None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=False) + "\n")


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
    layout_by_node = {
        node_index: layout_index
        for layout_index, node_index in enumerate(layout.get("nodeIndex", []))
    }
    parents = raw_nodes.get("parentIndex", [])
    types = raw_nodes.get("nodeType", [])
    names = raw_nodes.get("nodeName", [])
    values = raw_nodes.get("nodeValue", [])
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
                styles = {
                    name: decode_string(strings, value)
                    for name, value in zip(COMPUTED_STYLES, values_for_style)
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


def relative_error(reference: float, candidate: float) -> float:
    return abs(reference - candidate) / max(1.0, abs(reference))


def geometry_metrics(
    matches: list[tuple[BrowserNode, NativeNode, float]]
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    errors: list[float] = []
    for browser, native, _ in matches:
        if not browser.bounds or not native.bounds:
            continue
        labels = ("x", "y", "width", "height")
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
            "mean_relative_error": round(mean_error, 6),
        })
    mean_error = statistics.fmean(errors) if errors else 1.0
    return {
        "score": round(max(0.0, 1.0 - mean_error), 6),
        "matched_bounds": len(rows),
        "mean_relative_error": round(mean_error, 6),
        "rows": rows,
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
            if native_value and browser_value == native_value:
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
    foreground_distance = 0.0
    foreground_samples = 0
    differing = 0
    threshold = 2
    reference_background = Counter(ref_pixels).most_common(1)[0][0]
    candidate_background = Counter(candidate_pixels).most_common(1)[0][0]
    for ref_pixel, candidate_pixel in zip(ref_pixels, candidate_pixels):
        delta = sum(abs(a - b) for a, b in zip(ref_pixel[:3], candidate_pixel[:3]))
        total_distance += delta
        if delta > threshold:
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
    global_similarity = max(0.0, 1.0 - mean_channel_error / 255.0)
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


def compare_one(
    importer: Path,
    observer: Path,
    source: Path,
    output: Path,
    timeout_seconds: int,
    from_source: str,
    browser_path: Path | None,
    fixture_metadata: dict[str, Any] | None = None,
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
    source_args = ["--from", from_source, "--file", str(source)]
    browser_args = [
        str(importer), *source_args, "--mode", "baked", "--emit", "ir-json",
        "--output", str(browser_ir),
    ]
    if browser_path:
        browser_args.extend(["--browser", str(browser_path)])
    native_args = [
        str(importer), *source_args, "--offline", "--mode", "baked",
        "--emit", "ir-json", "--output", str(native_ir),
    ]
    if from_source == "claude":
        native_args.append("--execute-bundle")
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

    browser_doc = json.loads(browser_ir.read_text())
    native_doc = json.loads(native_ir.read_text())
    # The lab artifacts are designed to be shareable: retain source identity by
    # basename + hash, never by a machine-local absolute path.
    browser_doc["sourceFile"] = source.name
    native_doc["sourceFile"] = source.name
    write_json(browser_ir, browser_doc)
    write_json(native_ir, native_doc)
    evidence = find_browser_evidence(browser_ir)
    capture = json.loads((evidence / "capture.json").read_text())
    viewport = capture["provenance"]["viewport"]["resolved"]
    capture_width, capture_height = capture_logical_size(capture)
    render_args = [
        str(observer), "--input", str(native_ir),
        "--render", str(native_render), "--layout", str(native_layout),
        "--width", str(capture_width), "--height", str(capture_height),
        "--scale", str(capture["reference"]["device_scale_factor"]),
    ]
    render_run = run_command(render_args, timeout_seconds)
    write_json(native_dir / "render-command.json", command_record(render_run))
    if render_run.returncode != 0:
        raise LabError(
            f"native render failed ({render_run.returncode}): {render_run.stderr}")

    for name in (
        ".pulp-browser-capture-v1", "browser.png", "capture.json",
        "dom-snapshot.json", "semantic-report.json", "tokens.json",
        "interaction-report.json",
    ):
        source_evidence = evidence / name
        if source_evidence.exists():
            shutil.copy2(source_evidence, browser_dir / name)
    recognition = source_recognition(source.read_text(errors="replace"))
    browser_snapshot = json.loads((browser_dir / "dom-snapshot.json").read_text())
    native_layout_doc = json.loads(native_layout.read_text())
    browser_flat = browser_nodes(browser_snapshot)
    native_flat = native_nodes(native_doc, native_layout_doc)
    matches, missing, extra = match_nodes(browser_flat, native_flat)
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
    denominator = max(1, len(browser_flat))
    precision_denominator = max(1, len(native_flat))
    structurally_valid_matches = max(0, len(matches) - len(parentage_errors))
    recall = structurally_valid_matches / denominator
    precision = structurally_valid_matches / precision_denominator
    structural_score = (
        2 * precision * recall / (precision + recall)
        if precision + recall else 0.0)
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
    geometry = geometry_metrics(matches)
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
        },
        "source_recognition": recognition,
        "unsupported_dynamic_features": blockers,
        "timings": timings,
        "comparison": {
            "structural": structural,
            "geometry": geometry,
            "typography": typography,
            "visual": visual,
        },
        "classifications": classifications,
        "promotion": recommendation,
        "canonical_output_modified": False,
    }
    write_json(comparison_dir / "report.json", report)
    write_json(comparison_dir / "structural-diff.json", structural)
    write_json(comparison_dir / "classifications.json", classifications)
    (comparison_dir / "summary.md").write_text(format_summary(report))
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
        "ranked_gaps": ranked,
    }
