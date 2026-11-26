"""Run Yoga layout fixtures on Vellum's layout through vellum-app-host.

A Yoga fixture (``fixtures/yoga/*.json``) describes a tree of boxes with
Yoga-style properties and a viewport. This module maps the properties Vellum's
authoring layout has an equivalent for onto a Vellum bundle, runs the bundle
headlessly with ``--dump-tree``, and turns the tree dump back into the node
rectangles the Yoga goldens record.

The mapping is deliberately narrow. A property with no Vellum equivalent
raises ``Unsupported`` naming it, so a fixture is reported as not measured
rather than approximated and compared. Mapping table:

=============================  ==========================================
Yoga fixture style             Vellum style
=============================  ==========================================
direction row / column         direction horizontal / vertical
width, height (px)             width, height
width, height ("N%")           px against the parent's content box
padding                        padding
flex_grow                      flexGrow
align_items start              (Vellum's only behaviour, nothing emitted)
position absolute, left, top   x, y (non-root only)
overflow                       overflow
z_index                        ignored (paint order, not geometry)
=============================  ==========================================

Not mapped, so refused: flex_shrink, flex_basis, aspect_ratio, border_width,
align_items other than start (Vellum's authoring layout accepts an ``align``
key but does not move children for it), and every other key.

Vellum's host refuses a blank frame, so the mapped tree carries one decorative
absolutely positioned gradient node (``DECOR_ID``) that takes no flow space and
is dropped from the snapshot.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
from typing import Any

HOST_ENV = "VELLUM_APP_HOST"
HOST_TIMEOUT_SECONDS = 90

_DIRECTIONS = {"row": "horizontal", "column": "vertical"}
DECOR_ID = "vellum-layout-decor"
_FILL = ["#14B8A6", "#F59E0B", "#EF4444", "#8B5CF6"]
_IGNORED = {"z_index"}


class Unsupported(Exception):
    """The fixture uses a property Vellum's layout has no equivalent for."""


def _length(value: Any, basis: float, key: str) -> float:
    if isinstance(value, bool):
        raise Unsupported(f"{key}={value!r}")
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str) and value.endswith("%"):
        try:
            return basis * float(value[:-1]) / 100.0
        except ValueError:
            pass
    raise Unsupported(f"{key}={value!r}")


def _map_node(node: dict[str, Any], parent_box: tuple[float, float], root: bool) -> dict[str, Any]:
    style = node.get("style", {})
    mapped: dict[str, Any] = {}
    absolute = style.get("position") == "absolute"
    for key, value in style.items():
        if key in _IGNORED or key == "position":
            if key == "position" and value not in ("absolute", "relative"):
                raise Unsupported(f"position={value!r}")
            continue
        if key == "direction":
            if value not in _DIRECTIONS:
                raise Unsupported(f"direction={value!r}")
            mapped["direction"] = _DIRECTIONS[value]
        elif key in ("width", "height"):
            basis = parent_box[0] if key == "width" else parent_box[1]
            mapped[key] = _length(value, basis, key)
        elif key == "padding":
            mapped["padding"] = _length(value, parent_box[0], key)
        elif key == "flex_grow":
            mapped["flexGrow"] = _length(value, 0.0, key)
        elif key == "align_items":
            if value != "start":
                raise Unsupported(f"align_items={value!r}")
        elif key in ("left", "top"):
            if not absolute or root:
                raise Unsupported(f"{key} without position absolute")
            mapped["x" if key == "left" else "y"] = _length(value, parent_box[0], key)
        elif key == "overflow":
            mapped["overflow"] = value
        else:
            raise Unsupported(f"{key}={value!r}")
    width = mapped.get("width", parent_box[0])
    height = mapped.get("height", parent_box[1])
    pad = mapped.get("padding", 0.0)
    content = (max(0.0, width - 2 * pad), max(0.0, height - 2 * pad))
    children = [_map_node(child, content, False) for child in node.get("children", [])]
    return {
        "type": "stack" if root or children else "view",
        "id": node["id"],
        "style": mapped,
        "children": children,
    }


def map_fixture(fixture: dict[str, Any]) -> dict[str, Any]:
    """The Vellum retained tree for a Yoga fixture, or ``Unsupported``."""
    viewport = fixture["viewport"]
    size = (float(viewport["w"]), float(viewport["h"]))
    tree = _map_node(fixture["tree"], size, True)
    tree["style"]["width"], tree["style"]["height"] = size
    _decorate(tree, [0])
    tree["children"].append({
        "type": "view", "id": DECOR_ID, "children": [],
        "style": {"x": 0, "y": 0, "width": size[0], "height": min(8.0, size[1]),
                  "backgroundLinearGradient": {"angle": 90, "stops": [
                      {"position": 0.01, "color": "#F8FAFC"}, {"position": 1, "color": "#EF4444"}]}},
    })
    return tree


def _decorate(node: dict[str, Any], counter: list[int]) -> None:
    """Colour every node so the frame has content; colour never moves a box."""
    node["style"]["backgroundColor"] = "#0F172A" if counter[0] == 0 else _FILL[counter[0] % 4]
    counter[0] += 1
    for child in node["children"]:
        _decorate(child, counter)


def bundle_for(tree: dict[str, Any]) -> str:
    """A static authoring bundle that renders ``tree`` once."""
    document = json.dumps({"protocol": "vellum.authoring-host.v1", "tree": tree})
    return (
        "(() => {\n"
        "  let dirty = true;\n"
        f"  const doc = {json.dumps(document)};\n"
        "  const render = () => { dirty = false; return doc; };\n"
        "  globalThis.__vellum = { protocol: \"vellum.authoring-host.v1\","
        " hostProtocol: \"vellum.authoring-host.v2\", renderJSON: render,"
        " dispatchJSON: render, isDirty() { return dirty; },"
        " pumpJSON() { dirty = false; return JSON.stringify({"
        " protocol: \"vellum.authoring-host.v2\", kind: \"render-result\","
        " revision: 0, tree: JSON.parse(doc).tree }); } };\n"
        "})();\n"
    )


def snapshot_from_dump(fixture: dict[str, Any], dump: dict[str, Any]) -> dict[str, Any]:
    """The Yoga snapshot shape (node ids and rects) from a tree dump."""
    return {
        "fixture": fixture.get("id"),
        "viewport": fixture["viewport"],
        "nodes": [
            {"id": n["id"], "rect": {"x": n["box"]["x"], "y": n["box"]["y"],
                                     "w": n["box"]["width"], "h": n["box"]["height"]}}
            for n in dump["nodes"] if n["id"] != DECOR_ID
        ],
    }


def run_host(host: Path, tree: dict[str, Any]) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vellum-layout-") as scratch:
        base = Path(scratch).resolve()
        bundle = base / "layout.js"
        dump = base / "tree.json"
        bundle.write_text(bundle_for(tree), encoding="utf-8")
        done = subprocess.run(
            [str(host), "--bundle", str(bundle), "--no-window", "--dump-tree", str(dump)],
            check=False, capture_output=True, text=True, timeout=HOST_TIMEOUT_SECONDS,
            env=dict(os.environ, MTL_DEBUG_LAYER="0"),
        )
        if done.returncode != 0:
            raise RuntimeError(f"host exited {done.returncode}: {done.stderr.strip()}")
        return json.loads(dump.read_text(encoding="utf-8"))


def compare_rects(expected: dict[str, Any], actual: dict[str, Any],
                  tolerance: float) -> list[str]:
    """Differences between node ids and rects; empty when they agree."""
    want = {n["id"]: n["rect"] for n in expected["nodes"]}
    got = {n["id"]: n["rect"] for n in actual["nodes"]}
    problems = []
    for node_id in sorted(set(want) | set(got)):
        if node_id not in got:
            problems.append(f"{node_id}: missing from the Vellum tree")
        elif node_id not in want:
            problems.append(f"{node_id}: not in the golden")
        else:
            for axis in ("x", "y", "w", "h"):
                if abs(float(want[node_id][axis]) - float(got[node_id][axis])) > tolerance:
                    problems.append(
                        f"{node_id}.{axis}: golden {want[node_id][axis]} vs vellum {got[node_id][axis]}")
    return problems
