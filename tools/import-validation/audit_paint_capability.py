#!/usr/bin/env python3
"""audit_paint_capability.py — can Pulp draw this captured panel, node by node?

This is a CAPABILITY audit, not a fidelity one. It answers "is there a path that
lowers this node's declared appearance" by reading the capability ledger
(``compat.json``); it says nothing about whether the result matches Chrome. Treat
its output as a LOWER BOUND on divergence — the pixel score
(``score_native_panel.py``) is the thing that measures agreement.

Four rules exist because the first version of this audit reported 0-3 unpaintable
nodes per design and every one of them was an artifact of how it counted:

1. INK-BEARING DENOMINATOR. A node that paints nothing cannot be drawn wrongly,
   and 56% of one corpus design's nodes are pure layout containers. Counting them
   as successes measured how many spacer divs a generator emits.

2. AREA WEIGHTING ALONGSIDE THE COUNT. Per-node and per-pixel disagree, and not
   marginally: a design can score 1% of nodes unpaintable while that 1% is the
   whole screen. Both are reported, and neither is allowed to stand alone.

3. SVG SUBTREES ATTRIBUTED TO THE SVG CAUSE. An ``<svg>`` root classifies as
   needing an asset, but its ``path`` / ``circle`` / ``rect`` children have
   ordinary CSS boxes, so style classification returns "drawable" for geometry no
   style can express. Every descendant of an SVG is attributed to SVG.

4. VALUE FORMS, NOT PROPERTY NAMES. A ledger entry saying `box-shadow: supported`
   does not mean this node's box-shadow is supported. Each captured value is
   tested against the entry's ``unsupportedValueForms`` regexes. Without this the
   predicate cannot fire at all, which is the defect that produced the original
   near-zero numbers.

A property the capture records but the ledger has no entry for is a MEASUREMENT
GAP, reported in its own bucket and counted as not-drawable — never silently
treated as fine.

Reported per design, never a mean (a mean over hundreds of nodes hides the single
node that is the whole panel):

    worst node      largest single non-drawable ink-bearing node, as % of panel area
    node pass rate  % of ink-bearing nodes classified drawable
    area failing    union area of non-drawable ink-bearing nodes, as % of panel area

The area number is a real union over a coverage grid, not a sum of boxes: nested
boxes overlap, and summing them can exceed the panel.

Exit codes:
    0  audited
    2  EX_INPUT    - missing / unreadable / malformed input
    5  EX_SCORE    - audited, and a gated number exceeded its threshold
    7  EX_HARNESS  - the audit could not run (a measurement gap, not a pass)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

EX_INPUT = 2
EX_SCORE = 5
EX_HARNESS = 7

# The property order a capture taken before `computedStyleNames` was recorded
# used. A snapshot that predates that field is still auditable, but only against
# this list, and the report says which protocol it read.
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
    "letter-spacing", "line-height", "text-transform", "text-decoration-line",
    "white-space", "cursor", "pointer-events",
]

# Values a node carries when it did not ask for the property. Testing these
# against an unsupported-value form would flag every node in the document for a
# declaration it never made.
INITIAL_VALUES = {
    "background-image": {"none"},
    "background-blend-mode": {"normal"},
    "background-size": {"auto"},
    "background-position": {"0% 0%"},
    "background-repeat": {"repeat"},
    "box-shadow": {"none"},
    "text-shadow": {"none"},
    "filter": {"none"},
    "backdrop-filter": {"none"},
    "clip-path": {"none"},
    "mask-image": {"none"},
    "mix-blend-mode": {"normal"},
    "isolation": {"auto"},
    "transform": {"none"},
    "font-variation-settings": {"normal"},
    "word-spacing": {"normal", "0px"},
    "text-decoration-line": {"none"},
    "text-decoration-thickness": {"auto"},
    "text-underline-offset": {"auto"},
    "border-top-style": {"none"},
    "border-right-style": {"none"},
    "border-bottom-style": {"none"},
    "border-left-style": {"none"},
    "content": {"normal", "none"},
    "overflow": {"visible"},
}

# Properties whose value cannot make a node draw wrongly on the frozen-geometry
# path: Chrome has already solved layout and the lowering copies its boxes, so a
# layout keyword the ledger cannot express costs nothing. Excluding them is a
# claim about this pipeline, not about Pulp's CSS support in general.
GEOMETRY_SOLVED_BY_CAPTURE = {"display", "position", "overflow", "z-index"}

# Elements whose paint is imperative or file-backed rather than declared in
# style, so no ledger entry can decide them.
ASSET_TAGS = {"img", "canvas", "video", "iframe", "picture"}

TRANSPARENT = re.compile(r"^\s*(?:transparent|rgba\(\s*0\s*,\s*0\s*,\s*0\s*,\s*0\s*\))\s*$", re.I)
ZERO_LENGTH = re.compile(r"^\s*0(?:px|%)?\s*$")


def fail(code: int, message: str) -> None:
    print(f"audit_paint_capability: {message}", file=sys.stderr)
    raise SystemExit(code)


# ── ledger ──────────────────────────────────────────────────────────────────

class Ledger:
    """The css/* section of compat.json, keyed by CSS property name."""

    def __init__(self, compat: dict, surface: str = "css") -> None:
        section = compat.get(surface)
        if not isinstance(section, dict):
            fail(EX_INPUT, f"compat.json has no {surface!r} section")
        self.surface = surface
        self.by_property: dict[str, dict] = {}
        for key, entry in section.items():
            if not isinstance(entry, dict):
                continue
            name = key.split("/", 1)[1]
            if name.startswith("__"):
                continue
            self.by_property[_kebab(name)] = entry

    def entry(self, prop: str) -> dict | None:
        return self.by_property.get(prop)

    def unsupported_reason(self, prop: str, value: str,
                           styles: dict[str, str]) -> str | None:
        """Why this concrete value cannot be drawn, or None if it can.

        A property with no entry is not decided here — the caller reports it as
        a measurement gap, which is a different thing from a known limit.
        """
        entry = self.by_property.get(prop)
        if entry is None:
            return None
        forms = entry.get("unsupportedValueForms") or []
        if not forms and entry.get("status") in ("missing", "wontfix", "noop"):
            # An entry with no declared forms says nothing about which values
            # are drawable, so an unwired property fails on any declared value.
            # An entry that DOES declare forms is more precise than its status:
            # `border-top-style` is unrouted, but `solid` is the only style Pulp
            # paints anyway, so a solid edge is drawn correctly by accident and
            # flagging it would manufacture a failure.
            return f"{prop}: ledger status {entry.get('status')}"
        for form in forms:
            guard = form.get("requires")
            if guard:
                other = styles.get(guard.get("property", ""), "")
                pattern = guard.get("notMatch")
                if pattern and re.search(pattern, other, re.I):
                    continue
                if not other:
                    continue
            if re.search(form["match"], value, re.I):
                return f"{prop}: {form['value']}"
        return None


def _kebab(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()


# ── capture ─────────────────────────────────────────────────────────────────

class Node:
    __slots__ = ("index", "tag", "styles", "box", "parent", "text",
                 "classification", "cause", "in_svg", "all_causes")

    def __init__(self, index: int, tag: str, styles: dict[str, str],
                 box: tuple[float, float, float, float] | None,
                 parent: int, text: str) -> None:
        self.index = index
        self.tag = tag
        self.styles = styles
        self.box = box
        self.parent = parent
        self.text = text
        self.classification = "native"
        self.cause = ""
        self.in_svg = False
        # Every property that would block this node, not only the first one
        # found. Reporting the first alone makes each fix look like it clears
        # the node it was blocking, when a second cause is waiting behind it.
        self.all_causes: list[str] = []


def load_capture(capture_dir: Path) -> tuple[list[Node], tuple[int, int], str]:
    snapshot_path = capture_dir / "dom-snapshot.json"
    if not snapshot_path.is_file():
        fail(EX_INPUT, f"no dom-snapshot.json in {capture_dir}")
    try:
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        fail(EX_INPUT, f"unreadable dom-snapshot.json: {exc}")

    strings = snapshot.get("strings") or []
    documents = snapshot.get("documents") or []
    if not documents:
        fail(EX_INPUT, "dom-snapshot.json has no documents")
    document = documents[0]

    names = snapshot.get("computedStyleNames")
    if names:
        protocol = f"declared ({len(names)} properties)"
    else:
        names = LEGACY_COMPUTED_STYLES
        protocol = (f"legacy ({len(names)} properties, inferred — the snapshot "
                    f"predates computedStyleNames)")

    def s(index: int) -> str:
        return strings[index] if 0 <= index < len(strings) else ""

    dom = document.get("nodes") or {}
    node_names = dom.get("nodeName") or []
    parents = dom.get("parentIndex") or []
    node_values = dom.get("nodeValue") or []

    layout = document.get("layout") or {}
    layout_node_index = layout.get("nodeIndex") or []
    layout_styles = layout.get("styles") or []
    layout_bounds = layout.get("bounds") or []

    nodes: list[Node] = []
    for slot, dom_index in enumerate(layout_node_index):
        row = layout_styles[slot] if slot < len(layout_styles) else []
        if len(row) != len(names):
            # A row that does not match the declared request order is a decoding
            # error, not a node to score. Refusing beats silently keying the
            # wrong property.
            if row:
                fail(EX_INPUT,
                     f"style row {slot} has {len(row)} values for "
                     f"{len(names)} declared properties")
            continue
        styles = {name: s(row[i]) for i, name in enumerate(names)}
        bounds = layout_bounds[slot] if slot < len(layout_bounds) else None
        box = tuple(bounds) if bounds and len(bounds) == 4 else None
        tag = (s(node_names[dom_index]) if dom_index < len(node_names) else "").lower()
        text = s(node_values[dom_index]) if dom_index < len(node_values) else ""
        parent = parents[dom_index] if dom_index < len(parents) else -1
        nodes.append(Node(dom_index, tag, styles, box, parent, text))

    if not nodes:
        fail(EX_HARNESS, "no laid-out nodes decoded — nothing to audit")

    # The panel is the surface the capture declared, not the scroll extent.
    # contentHeight on a page that overflows its viewport is the whole scrollable
    # document, which silently inflates the denominator — one corpus design reads
    # 2767px tall against a declared 800px panel, dividing every area number by
    # 3.5 and making a full-width faceplate look like a third of the surface.
    width = height = 0
    capture_json = capture_dir / "capture.json"
    if capture_json.is_file():
        try:
            viewport = (json.loads(capture_json.read_text(encoding="utf-8"))
                        .get("provenance", {}).get("viewport", {}))
            resolved = viewport.get("resolved") or viewport.get("initial") or {}
            width = int(resolved.get("width") or 0)
            height = int(resolved.get("height") or 0)
        except (OSError, ValueError, AttributeError, TypeError):
            width = height = 0
    if width <= 0 or height <= 0:
        width = int(document.get("contentWidth") or 0)
        height = int(document.get("contentHeight") or 0)
    if width <= 0 or height <= 0:
        maxx = max((n.box[0] + n.box[2] for n in nodes if n.box), default=0)
        maxy = max((n.box[1] + n.box[3] for n in nodes if n.box), default=0)
        width, height = int(maxx) or 1, int(maxy) or 1

    _mark_svg_subtrees(nodes, node_names, parents, strings)
    return nodes, (width, height), protocol


def _mark_svg_subtrees(nodes, node_names, parents, strings) -> None:
    """Attribute every descendant of an <svg> to SVG.

    The svg root itself classifies as needing an asset; its children have
    ordinary CSS boxes and would otherwise be classified by style, which returns
    "drawable" for path geometry that no CSS property expresses.
    """
    def name_of(index: int) -> str:
        if 0 <= index < len(node_names):
            si = node_names[index]
            return (strings[si] if 0 <= si < len(strings) else "").lower()
        return ""

    inside: dict[int, bool] = {}

    def resolve(index: int) -> bool:
        if index in inside:
            return inside[index]
        parent = parents[index] if 0 <= index < len(parents) else -1
        if parent is None or parent < 0:
            result = False
        else:
            result = name_of(parent) == "svg" or resolve(parent)
        inside[index] = result
        return result

    sys.setrecursionlimit(max(10000, sys.getrecursionlimit()))
    for node in nodes:
        node.in_svg = resolve(node.index)


# ── classification ──────────────────────────────────────────────────────────

def paints_ink(node: Node) -> bool:
    """Does this node put anything on the screen?

    Deliberately generous — a false "yes" only dilutes the pass rate, while a
    false "no" removes a node from the denominator entirely, which is the bug
    this whole rewrite exists to fix.
    """
    st = node.styles
    if st.get("visibility") in ("hidden", "collapse"):
        return False
    if st.get("display") == "none":
        return False
    opacity = st.get("opacity", "1")
    try:
        if float(opacity) <= 0.0:
            return False
    except ValueError:
        pass
    if node.box and (node.box[2] <= 0 or node.box[3] <= 0):
        return False

    if node.tag == "#text":
        return bool(node.text.strip())
    if node.tag in ASSET_TAGS or node.tag == "svg":
        return True

    bg = st.get("background-color", "")
    if bg and not TRANSPARENT.match(bg):
        return True
    for prop in ("background-image", "box-shadow", "text-shadow", "filter",
                 "backdrop-filter", "mask-image", "content"):
        value = st.get(prop, "")
        if value and value not in INITIAL_VALUES.get(prop, {"none"}):
            return True
    for edge in ("top", "right", "bottom", "left"):
        width = st.get(f"border-{edge}-width", "")
        colour = st.get(f"border-{edge}-color", "")
        if width and not ZERO_LENGTH.match(width) and not TRANSPARENT.match(colour or ""):
            return True
    outline_width = st.get("outline-width", "")
    outline_style = st.get("outline-style", "")
    if outline_width and not ZERO_LENGTH.match(outline_width) and outline_style not in ("", "none"):
        return True
    return False


def classify(node: Node, ledger: Ledger) -> None:
    if node.in_svg:
        node.classification, node.cause = "svg-geometry", "svg subtree"
        return
    if node.tag == "svg":
        node.classification, node.cause = "svg-geometry", "svg root"
        return
    if node.tag in ASSET_TAGS:
        node.classification, node.cause = "image-asset", f"<{node.tag}>"
        return

    gaps: list[str] = []
    blocked: list[str] = []
    for prop, value in node.styles.items():
        if prop in GEOMETRY_SOLVED_BY_CAPTURE:
            continue
        if not value or value in INITIAL_VALUES.get(prop, set()):
            continue
        if ledger.entry(prop) is None:
            gaps.append(prop)
            continue
        reason = ledger.unsupported_reason(prop, value, node.styles)
        if reason:
            blocked.append(reason)
    node.all_causes = [f"no ledger entry: {p}" for p in sorted(set(gaps))] + blocked
    if blocked:
        node.classification, node.cause = "unsupported-value", blocked[0]
        return
    if gaps:
        node.classification = "no-ledger-entry"
        node.cause = "no ledger entry for: " + ", ".join(sorted(set(gaps)))
        return
    node.classification, node.cause = "native", ""


# ── area ────────────────────────────────────────────────────────────────────

def union_area(boxes, size, cell: int = 4) -> float:
    """Union area of a set of boxes, in square CSS px.

    A coverage grid rather than a sum: nested boxes overlap, and a sum of a
    faceplate plus its children can exceed the panel it sits in. `cell` trades
    exactness for memory; at 4px a 1280x800 panel is 64000 cells.
    """
    width, height = size
    cols = max(1, (width + cell - 1) // cell)
    rows = max(1, (height + cell - 1) // cell)
    grid = bytearray(cols * rows)
    for x, y, w, h in boxes:
        x0 = max(0, int(x) // cell)
        y0 = max(0, int(y) // cell)
        x1 = min(cols, (int(x + w) + cell - 1) // cell)
        y1 = min(rows, (int(y + h) + cell - 1) // cell)
        for row in range(y0, y1):
            base = row * cols
            for col in range(x0, x1):
                grid[base + col] = 1
    return sum(grid) * cell * cell


# ── report ──────────────────────────────────────────────────────────────────

def audit(capture_dir: Path, compat_path: Path, label: str,
          surface: str) -> dict:
    try:
        compat = json.loads(compat_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        fail(EX_INPUT, f"unreadable {compat_path}: {exc}")
    ledger = Ledger(compat, surface)

    nodes, size, protocol = load_capture(capture_dir)
    for node in nodes:
        classify(node, ledger)

    ink = [n for n in nodes if paints_ink(n)]
    if not ink:
        fail(EX_HARNESS, f"{label}: no ink-bearing nodes — nothing to score")

    panel_area = float(size[0] * size[1])
    failing = [n for n in ink if n.classification != "native"]
    failing_boxes = [n.box for n in failing if n.box]

    worst_node = None
    worst_fraction = 0.0
    for node in failing:
        if not node.box:
            continue
        fraction = (node.box[2] * node.box[3]) / panel_area
        if fraction > worst_fraction:
            worst_fraction, worst_node = fraction, node

    by_cause: dict[str, dict] = {}
    for node in failing:
        bucket = by_cause.setdefault(
            node.classification, {"nodes": 0, "boxes": [], "examples": []})
        bucket["nodes"] += 1
        if node.box:
            bucket["boxes"].append(node.box)
        if len(bucket["examples"]) < 3 and node.cause:
            bucket["examples"].append(f"<{node.tag}> {node.cause}")

    causes = {}
    for name, bucket in sorted(by_cause.items()):
        causes[name] = {
            "nodes": bucket["nodes"],
            "area_pct_of_panel": round(
                100.0 * union_area(bucket["boxes"], size) / panel_area, 2),
            "examples": bucket["examples"],
        }

    # Independently of which cause was recorded first, how many ink-bearing
    # nodes each property blocks. Landing the top property does not release
    # every node it heads, and this is where that shows.
    per_property: dict[str, int] = {}
    for node in failing:
        for reason in dict.fromkeys(
                cause.split(":", 1)[0] for cause in node.all_causes):
            per_property[reason] = per_property.get(reason, 0) + 1

    return {
        "label": label,
        "capture": str(capture_dir),
        "surface": surface,
        "capture_protocol": protocol,
        "panel": {"width": size[0], "height": size[1]},
        "nodes_total": len(nodes),
        "nodes_ink_bearing": len(ink),
        "nodes_paint_nothing": len(nodes) - len(ink),
        "worst_node_pct_of_panel": round(100.0 * worst_fraction, 2),
        "worst_node": (f"<{worst_node.tag}> {worst_node.cause}"
                       if worst_node else None),
        "ink_nodes_passing_pct": round(
            100.0 * (len(ink) - len(failing)) / len(ink), 2),
        "area_failing_pct": round(
            100.0 * union_area(failing_boxes, size) / panel_area, 2),
        "causes": causes,
        "blocking_properties": dict(
            sorted(per_property.items(), key=lambda kv: -kv[1])),
    }


def print_report(report: dict) -> None:
    print(f"── {report['label']} "
          f"({report['panel']['width']}x{report['panel']['height']}, "
          f"surface={report['surface']})")
    print(f"   capture protocol: {report['capture_protocol']}")
    print(f"   nodes: {report['nodes_total']} laid out, "
          f"{report['nodes_ink_bearing']} ink-bearing "
          f"({report['nodes_paint_nothing']} paint nothing and are excluded)")
    print(f"   worst node      {report['worst_node_pct_of_panel']:6.2f}% of panel"
          f"   {report['worst_node'] or '—'}")
    print(f"   node pass rate  {report['ink_nodes_passing_pct']:6.2f}% of ink-bearing nodes")
    print(f"   area failing    {report['area_failing_pct']:6.2f}% of panel area")
    for name, bucket in report["causes"].items():
        print(f"     {name:<18} {bucket['nodes']:4d} nodes  "
              f"{bucket['area_pct_of_panel']:6.2f}% area")
        for example in bucket["examples"]:
            print(f"       · {example}")
    if report["blocking_properties"]:
        print("   blocking properties (a node can be blocked by more than one):")
        for prop, count in report["blocking_properties"].items():
            print(f"     {prop:<26} {count:4d} ink-bearing nodes")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--capture", required=True, action="append",
                        help="browser-capture dir (repeatable)")
    parser.add_argument("--label", action="append", default=[],
                        help="label for the matching --capture")
    parser.add_argument("--compat", default=None, help="path to compat.json")
    parser.add_argument("--surface", default="css",
                        help="capability ledger surface to score against "
                             "(default css — the el.style path the shipped "
                             "emitter writes to)")
    parser.add_argument("--json-out", default=None)
    parser.add_argument("--max-worst-node-pct", type=float, default=None,
                        help="fail if any design's worst node exceeds this")
    parser.add_argument("--max-area-failing-pct", type=float, default=None,
                        help="fail if any design's failing area exceeds this")
    args = parser.parse_args()

    compat_path = Path(args.compat) if args.compat else _find_compat()
    if compat_path is None or not compat_path.is_file():
        fail(EX_INPUT, "compat.json not found (pass --compat)")

    reports = []
    for i, capture in enumerate(args.capture):
        directory = Path(capture)
        if not directory.is_dir():
            fail(EX_INPUT, f"not a directory: {directory}")
        label = args.label[i] if i < len(args.label) else directory.name
        report = audit(directory, compat_path, label, args.surface)
        reports.append(report)
        print_report(report)

    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps({"designs": reports}, indent=2), encoding="utf-8")

    breached = []
    for report in reports:
        if (args.max_worst_node_pct is not None
                and report["worst_node_pct_of_panel"] > args.max_worst_node_pct):
            breached.append(f"{report['label']}: worst node "
                            f"{report['worst_node_pct_of_panel']}%")
        if (args.max_area_failing_pct is not None
                and report["area_failing_pct"] > args.max_area_failing_pct):
            breached.append(f"{report['label']}: failing area "
                            f"{report['area_failing_pct']}%")
    if breached:
        print("audit_paint_capability: over threshold: " + "; ".join(breached),
              file=sys.stderr)
        return EX_SCORE
    return 0


def _find_compat() -> Path | None:
    for directory in [Path.cwd(), *Path(__file__).resolve().parents]:
        candidate = directory / "compat.json"
        if candidate.is_file():
            return candidate
    return None


if __name__ == "__main__":
    sys.exit(main())
