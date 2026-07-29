#!/usr/bin/env python3
"""Translate the constrained Forge emission profile to canonical DesignIR v1."""

import argparse
import json
import re
from pathlib import Path


def designmd_colors(markdown):
    """Read the deliberately flat colors map used by the experiment prompts."""
    match = re.search(r"(?ms)^colors:\s*\n(?P<body>(?:^[ \t]+.+\n?)*)", markdown)
    colors = {}
    if not match:
        return colors
    for line in match.group("body").splitlines():
        item = re.match(r"\s{2,}([A-Za-z0-9_.-]+):\s*[\"']?(#[0-9A-Fa-f]{6,8})", line)
        if item:
            colors[item.group(1)] = item.group(2)
    return colors


def resolve(value, colors):
    if not isinstance(value, str):
        return value
    token = re.fullmatch(r"\{colors\.([A-Za-z0-9_.-]+)\}", value)
    return colors.get(token.group(1), value) if token else value


def node_to_ir(node, colors):
    style = node.get("style", {})
    layout = node.get("layout", {})
    padding = layout.get("padding", [])
    if isinstance(padding, (int, float)):
        padding = [padding] * 4
    elif len(padding) == 1:
        padding *= 4
    border, shadow = style.get("border", {}), style.get("shadow", {})
    out_style = {}
    mapping = {
        "width": "width", "height": "height", "corner_radius": "borderRadius",
        "color": "color", "font_size": "fontSize", "font_weight": "fontWeight",
        "font_family": "fontFamily", "letter_spacing": "letterSpacing",
    }
    for source, target in mapping.items():
        if source in style:
            out_style[target] = resolve(style[source], colors)
    if "background" in style:
        out_style["backgroundColor"] = resolve(style["background"], colors)
    if "gradient" in style:
        out_style["backgroundGradient"] = style["gradient"]
    if border:
        out_style.update(borderWidth=border.get("width", 0),
                         borderColor=resolve(border.get("color", ""), colors),
                         borderStyle="solid")
    if shadow:
        out_style["boxShadow"] = (
            f"0px {shadow.get('y', 0)}px {shadow.get('blur', 0)}px "
            f"{resolve(shadow.get('color', '#00000000'), colors)}"
        )
    align = {"start": "flex-start", "end": "flex-end",
             "between": "space-between"}
    out_layout = {
        "display": "flex",
        "direction": layout.get("direction", "column"),
        "gap": layout.get("gap", 0),
        "justify": align.get(layout.get("justify"), layout.get("justify", "flex-start")),
        "align": align.get(layout.get("align"), layout.get("align", "stretch")),
        "wrap": False,
        "widthMode": "fixed" if "width" in style else "fill",
        "heightMode": "fixed" if "height" in style else "hug",
    }
    if padding:
        for key, value in zip(("paddingTop", "paddingRight", "paddingBottom",
                               "paddingLeft"), padding):
            out_layout[key] = value
    if "grow" in layout:
        out_layout["flexGrow"] = layout["grow"]
    control = node.get("control")
    ir = {
        "type": node["type"], "name": node.get("name", node["id"]),
        "style": out_style, "layout": out_layout,
        "stable_anchor_id": node["id"], "source_node_id": node["id"],
        "attributes": {}, "children": [node_to_ir(c, colors)
                                       for c in node.get("children", [])],
    }
    if node["type"] == "text":
        ir["content"] = node["text"]
    if control:
        ir["audioWidget"] = control["widget"]
        ir["label"] = control.get("label") or control["bind"].replace("_", " ").upper()
        ir["attributes"] = {
            "binding": control["bind"], "pulpShowInternalLabel": "false"
        }
    if not ir["attributes"]:
        del ir["attributes"]
    return ir


def translate(emission):
    colors = designmd_colors(emission["design_language"]["designmd"])
    return {
        "version": 1, "source": "designmd",
        "capture_method": "adapter_parse", "source_adapter": "forge-emission-python",
        "source_version": "1", "root": node_to_ir(emission["design"], colors),
        "tokens": {"colors": colors, "dimensions": {}, "strings": {
            "language.base": emission["design_language"]["base"],
            "layout.archetype": emission["layout"]["archetype"],
            "layout.hero": emission["layout"]["hero"],
        }},
        "assetManifest": {"version": 1, "assets": []},
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = translate(json.loads(args.artifact.read_text()))
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
