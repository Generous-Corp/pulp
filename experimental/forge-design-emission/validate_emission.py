#!/usr/bin/env python3
"""Python mirror of Forge's L0-L4 emission ladder for experiment telemetry."""

import argparse
import json
import math
import re
from collections import Counter
from pathlib import Path

from profile_to_design_ir import designmd_colors, translate

TOP_KEYS = {"design_language", "layout", "design", "notes"}
NODE_KEYS = {"id", "type", "name", "text", "style", "layout", "control", "children"}
STYLE_KEYS = {"width", "height", "background", "gradient", "corner_radius",
              "border", "shadow", "color", "font_size", "font_weight",
              "font_family", "letter_spacing"}
LAYOUT_KEYS = {"direction", "gap", "padding", "align", "justify", "grow"}
CONTROL_KEYS = {"bind", "widget", "label"}
ARCHETYPES = {"hero_knob", "pedal", "channel_strip", "xy_pad", "fader_bank",
              "rack_module", "radial", "feature_frame", "screen_console",
              "voice_panel", "grid_tile", "utility_grid"}
FONTS = {"Jost", "Inter", "JetBrains Mono"}
COLORS = {"background", "color"}
ID_RE = re.compile(r"^[a-z][a-z0-9_]{0,31}$")
COLOR_RE = re.compile(r"^(#[0-9a-fA-F]{6}(?:[0-9a-fA-F]{2})?|\{[^{}]+\})$")
TOKEN_RE = re.compile(r"\{colors\.([A-Za-z0-9_.-]+)\}")


def diagnostic(layer, path, message):
    return {"layer": layer, "path": path, "message": message}


def unknown(obj, allowed, layer, path, what):
    return [diagnostic(layer, path, f"unknown {what} field '{key}'")
            for key in obj if key not in allowed]


def valid_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def walk(node, state, path="design", depth=0):
    errors = []
    if not isinstance(node, dict):
        return [diagnostic("L1", path, "node is not an object")]
    state["count"] += 1
    if state["count"] > 120:
        errors.append(diagnostic("L1", path, "more than 120 nodes"))
    if depth > 8:
        errors.append(diagnostic("L1", path, "nested deeper than 8"))
    errors += unknown(node, NODE_KEYS, "L1", path, "node")
    node_id = node.get("id")
    node_path = node_id if isinstance(node_id, str) and node_id else path
    if not isinstance(node_id, str) or not ID_RE.fullmatch(node_id):
        errors.append(diagnostic("L1", node_path, "id must match [a-z][a-z0-9_]{0,31}"))
    elif node_id in state["ids"]:
        errors.append(diagnostic("L1", node_path, f"duplicate id '{node_id}'"))
    else:
        state["ids"].add(node_id)
    kind = node.get("type")
    if kind not in {"frame", "text", "ellipse"}:
        errors.append(diagnostic("L1", node_path, "type must be frame, text or ellipse"))
    if kind == "text" and not node.get("text"):
        errors.append(diagnostic("L1", node_path, "a text node needs non-empty 'text'"))
    style = node.get("style", {})
    if not isinstance(style, dict):
        errors.append(diagnostic("L1", node_path, "style must be an object"))
        style = {}
    errors += unknown(style, STYLE_KEYS, "L1", node_path, "style")
    for key in ("width", "height", "corner_radius", "font_size", "font_weight",
                "letter_spacing"):
        if key in style and not valid_number(style[key]):
            errors.append(diagnostic("L1", node_path, f"{key} must be a number in px, unquoted"))
    if style.get("font_family") and style["font_family"] not in FONTS:
        errors.append(diagnostic("L1", node_path, "font_family is not bundled"))
    for key in COLORS:
        if key in style and style[key] and not COLOR_RE.fullmatch(style[key]):
            errors.append(diagnostic("L1", node_path, f"style.{key} must be #RRGGBB, #RRGGBBAA or token"))
        if isinstance(style.get(key), str) and style[key].startswith("#"):
            state["raw_hexes"].add(style[key].lower())
    for key in ("border", "shadow"):
        part = style.get(key)
        if part is not None and not isinstance(part, dict):
            errors.append(diagnostic("L1", node_path, f"style.{key} must be an object"))
    gradient = style.get("gradient", "")
    if len(gradient) > 256:
        errors.append(diagnostic("L1", node_path, "gradient string too long"))
    if gradient.count(",") > 6:
        errors.append(diagnostic("L1", node_path, "gradient has more than 6 stops"))
    state["raw_hexes"].update(x.lower() for x in re.findall(r"#[0-9A-Fa-f]{6,8}", gradient))
    layout = node.get("layout", {})
    if not isinstance(layout, dict):
        errors.append(diagnostic("L1", node_path, "layout must be an object"))
        layout = {}
    errors += unknown(layout, LAYOUT_KEYS, "L1", node_path, "layout")
    if layout.get("direction") not in (None, "row", "column"):
        errors.append(diagnostic("L1", node_path, "layout.direction must be row or column"))
    padding = layout.get("padding")
    if isinstance(padding, list) and len(padding) not in (1, 4):
        errors.append(diagnostic("L1", node_path, "layout.padding takes one value or four"))
    control = node.get("control")
    if control is not None:
        if not isinstance(control, dict):
            errors.append(diagnostic("L1", node_path, "control must be an object"))
        else:
            errors += unknown(control, CONTROL_KEYS, "L1", node_path, "control")
            if control.get("widget") not in {"knob", "fader"}:
                errors.append(diagnostic("L1", node_path, "control.widget must be knob or fader"))
            if not control.get("bind"):
                errors.append(diagnostic("L1", node_path, "control.bind must name one macro"))
            else:
                state["binds"].append((control["bind"], node_path))
    children = node.get("children", [])
    if not isinstance(children, list):
        return errors + [diagnostic("L1", node_path, "children must be an array")]
    if len(children) > 24:
        errors.append(diagnostic("L1", node_path, "more than 24 children"))
    for child in children:
        errors += walk(child, state, node_path, depth + 1)
    return errors


def geometry(node, root_width, root_height):
    errors = []
    style = node.get("style", {})
    path = node.get("id", "design")
    for key in ("width", "height"):
        value = style.get(key)
        if value is not None and (not valid_number(value) or value < 0):
            errors.append(diagnostic("L4", path, f"style.{key} must be finite and non-negative"))
    if style.get("width", 0) > root_width or style.get("height", 0) > root_height:
        errors.append(diagnostic("L4", path, "fixed child size exceeds root"))
    if node.get("control"):
        if style.get("width", 0) < 40 or style.get("height", 0) < 40:
            errors.append(diagnostic("L4", path, "controls must be at least 40x40"))
    if node.get("type") == "text" and style.get("font_size", 0) < 10:
        errors.append(diagnostic("L4", path, "text font_size must be at least 10"))
    for child in node.get("children", []):
        errors += geometry(child, root_width, root_height)
    return errors


def validate(artifact, concept):
    diagnostics = []
    if not isinstance(artifact, dict):
        return {"layers": {"L0": False}, "diagnostics": [diagnostic("L0", "", "output is not one object")]}
    diagnostics += unknown(artifact, TOP_KEYS, "L0", "", "top-level")
    dl, layout, design = artifact.get("design_language"), artifact.get("layout"), artifact.get("design")
    if not isinstance(dl, dict) or not dl.get("base") or not dl.get("designmd"):
        diagnostics.append(diagnostic("L0", "design_language", "base and full designmd are required"))
    if not isinstance(layout, dict) or layout.get("archetype") not in ARCHETYPES:
        diagnostics.append(diagnostic("L0", "layout", "archetype must be registered"))
    if not isinstance(layout, dict) or not layout.get("hero"):
        diagnostics.append(diagnostic("L0", "layout", "hero must name a macro or be none"))
    if not isinstance(design, dict):
        diagnostics.append(diagnostic("L0", "design", "missing the design tree"))
    layers = {"L0": not any(d["layer"] == "L0" for d in diagnostics)}
    state = {"count": 0, "ids": set(), "raw_hexes": set(), "binds": []}
    if layers["L0"]:
        diagnostics += walk(design, state)
        if len(state["raw_hexes"]) > 8:
            diagnostics.append(diagnostic("L1", "design", f"uses {len(state['raw_hexes'])} raw hex colours; max 8"))
    layers["L1"] = layers["L0"] and not any(d["layer"] == "L1" for d in diagnostics)
    colors = designmd_colors(dl.get("designmd", "") if isinstance(dl, dict) else "")
    refs = set(TOKEN_RE.findall(json.dumps(design)))
    for ref in sorted(refs - colors.keys()):
        diagnostics.append(diagnostic("L2", f"colors.{ref}", f"unresolved token reference colors.{ref}"))
    if not colors:
        diagnostics.append(diagnostic("L2", "design_language.designmd", "DESIGN.md emitted no colors"))
    layers["L2"] = layers["L1"] and not any(d["layer"] == "L2" for d in diagnostics)
    macro_counts = Counter(bind for bind, _ in state["binds"])
    missing = sorted(set(concept["macros"]) - macro_counts.keys())
    unknown_binds = sorted(macro_counts.keys() - set(concept["macros"]))
    duplicate = sorted(k for k, count in macro_counts.items() if count != 1)
    if missing or unknown_binds or duplicate:
        diagnostics.append(diagnostic("L3", "design", f"binding diff missing={missing} unknown={unknown_binds} non_unique={duplicate}"))
    hero = layout.get("hero") if isinstance(layout, dict) else None
    if hero != "none" and macro_counts.get(hero) != 1:
        diagnostics.append(diagnostic("L3", "layout.hero", "hero must be bound exactly once"))
    layers["L3"] = layers["L2"] and not any(d["layer"] == "L3" for d in diagnostics)
    if isinstance(design, dict):
        root_style = design.get("style", {})
        width, height = root_style.get("width", 0), root_style.get("height", 0)
        if not valid_number(width) or not 360 <= width <= 1600:
            diagnostics.append(diagnostic("L4", "design.style.width", "root width must be 360..1600"))
        if not valid_number(height) or not 240 <= height <= 1000:
            diagnostics.append(diagnostic("L4", "design.style.height", "root height must be 240..1000"))
        if valid_number(width) and valid_number(height):
            diagnostics += geometry(design, width, height)
    layers["L4"] = layers["L3"] and not any(d["layer"] == "L4" for d in diagnostics)
    try:
        translated = translate(artifact)
        layers["L5-python"] = bool(translated.get("root"))
    except Exception as exc:
        layers["L5-python"] = False
        diagnostics.append(diagnostic("L5", "design", f"translator failed: {exc}"))
    return {
        "layers": layers, "diagnostics": diagnostics, "node_count": state["count"],
        "raw_hex_count": len(state["raw_hexes"]), "bind_counts": macro_counts,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--concept", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        artifact = json.loads(args.artifact.read_text())
    except Exception as exc:
        result = {"layers": {"L0": False}, "diagnostics": [diagnostic("L0", "", f"invalid JSON: {exc}")]}
    else:
        result = validate(artifact, json.loads(args.concept.read_text()))
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
