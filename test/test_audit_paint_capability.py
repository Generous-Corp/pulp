#!/usr/bin/env python3
"""Unit tests for tools/import-validation/audit_paint_capability.py.

The audit exists because its predecessor could not fail: every relevant ledger
entry carried an empty `unsupportedValues`, so the value-form check never fired
and every node came back drawable. These tests are therefore written two-sided —
each one that proves a form FIRES is paired with one proving it does NOT fire on
a value the same entry supports. A one-sided test would pass just as happily
against a predicate hardwired to "unsupported".

The last test is the ledger's own invariant: every machine-readable form must
name a value string that also appears verbatim in the human-readable
`unsupportedValues` list, so the two cannot drift apart.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "import-validation" / "audit_paint_capability.py"
COMPAT = REPO / "compat.json"

_spec = importlib.util.spec_from_file_location("audit_paint_capability", TOOL)
audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(audit)

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def ledger() -> "audit.Ledger":
    return audit.Ledger(json.loads(COMPAT.read_text(encoding="utf-8")), "css")


# ── the value-form predicate, both directions ───────────────────────────────

def test_box_shadow_colour_first_fires_author_order_does_not() -> None:
    led = ledger()
    computed = "rgba(0, 0, 0, 0.5) 0px 1px 2px 0px"
    authored = "0px 1px 2px rgba(0, 0, 0, 0.5)"
    check(led.unsupported_reason("box-shadow", computed, {}) is not None,
          "the colour-first form CSS actually serializes must be flagged")
    check(led.unsupported_reason("box-shadow", authored, {}) is None,
          "the author-order form the shim parses must NOT be flagged")


def test_box_shadow_second_layer_fires_single_layer_does_not() -> None:
    led = ledger()
    two = "0px 1px 2px rgba(0, 0, 0, 0.5), 0px 8px 24px rgba(0, 0, 0, 0.4)"
    one = "0px 1px 2px rgba(0, 0, 0, 0.5)"
    check(led.unsupported_reason("box-shadow", two, {}) is not None,
          "a second comma-separated shadow layer must be flagged")
    check(led.unsupported_reason("box-shadow", one, {}) is None,
          "a single layer must not be flagged as multi-layer")
    # The commas inside rgba() are argument separators, not layer separators.
    check(led.unsupported_reason("box-shadow", "0px 0px 4px rgb(1, 2, 3)", {}) is None,
          "commas inside a colour function must not read as a second layer")


def test_background_image_url_fires_gradient_stack_does_not() -> None:
    led = ledger()
    check(led.unsupported_reason(
        "background-image", 'url("panel.png")', {}) is not None,
        "a url() background layer must be flagged")
    # Multiple gradient layers ARE drawable — apply_css_background_gradient
    # splits top-level commas and keeps the whole ordered stack.
    stacked = ("linear-gradient(0deg, #000 0%, #111 100%), "
               "radial-gradient(circle, #222 0%, #333 100%)")
    check(led.unsupported_reason("background-image", stacked, {}) is None,
          "a multi-layer gradient stack must NOT be flagged")


def test_backdrop_filter_non_blur_fires_blur_does_not() -> None:
    led = ledger()
    check(led.unsupported_reason(
        "backdrop-filter", "blur(8px) saturate(1.4)", {}) is not None,
        "a non-blur function chained after a blur must be flagged")
    check(led.unsupported_reason("backdrop-filter", "blur(8px)", {}) is None,
          "a bare blur must not be flagged")


def test_clip_path_shapes_fire_path_does_not() -> None:
    led = ledger()
    for value in ("inset(0px round 12px)", "circle(50%)",
                  "polygon(0 0, 100% 0, 50% 100%)", "url(#clip)"):
        check(led.unsupported_reason("clip-path", value, {}) is not None,
              f"clip-path {value!r} must be flagged")
    check(led.unsupported_reason('clip-path', 'path("M 0 0 L 10 0")', {}) is None,
          "clip-path path() must not be flagged")


def test_background_size_guard_needs_an_image_present() -> None:
    """A sized background is only wrong when there is a background to size."""
    led = ledger()
    with_image = {"background-image": "linear-gradient(0deg, #000, #fff)"}
    without = {"background-image": "none"}
    check(led.unsupported_reason("background-size", "12px 12px", with_image) is not None,
          "a tiled background-size alongside a gradient must be flagged")
    check(led.unsupported_reason("background-size", "12px 12px", without) is None,
          "background-size with no background-image must not be flagged")


def test_border_style_flags_dashed_but_not_solid() -> None:
    """An unwired property is not automatically a wrong render.

    `border-top-style` has no route at all, but `solid` is the only style Pulp
    paints, so a solid edge comes out right regardless. Flagging it would
    manufacture a failure out of a correct render.
    """
    led = ledger()
    check(led.unsupported_reason("border-top-style", "dashed", {}) is not None,
          "a dashed edge must be flagged — it paints solid")
    check(led.unsupported_reason("border-top-style", "solid", {}) is None,
          "a solid edge must not be flagged")


def test_unknown_property_is_not_silently_drawable() -> None:
    led = ledger()
    check(led.entry("nonexistent-property") is None,
          "an unknown property must have no entry, so the caller reports a gap")


# ── denominator and attribution ─────────────────────────────────────────────

def _snapshot(nodes: list[dict], width: int = 100, height: int = 100) -> Path:
    """Build a minimal DOMSnapshot with the properties the audit reads."""
    names = ["display", "visibility", "opacity", "background-color",
             "background-image", "box-shadow", "border-top-width",
             "border-top-color", "border-top-style"]
    strings: list[str] = []

    def intern(value: str) -> int:
        if value not in strings:
            strings.append(value)
        return strings.index(value)

    node_names, parents, values = [], [], []
    layout_index, layout_styles, layout_bounds = [], [], []
    for i, node in enumerate(nodes):
        node_names.append(intern(node["tag"]))
        parents.append(node.get("parent", -1))
        values.append(intern(node.get("text", "")))
        layout_index.append(i)
        style = {**{n: "" for n in names}, **node.get("styles", {})}
        layout_styles.append([intern(style[n]) for n in names])
        layout_bounds.append(node.get("box", [0, 0, 10, 10]))

    document = {
        "contentWidth": width, "contentHeight": height,
        "nodes": {"nodeName": node_names, "parentIndex": parents,
                  "nodeValue": values},
        "layout": {"nodeIndex": layout_index, "styles": layout_styles,
                   "bounds": layout_bounds},
    }
    directory = Path(tempfile.mkdtemp())
    (directory / "dom-snapshot.json").write_text(json.dumps({
        "strings": strings, "computedStyleNames": names,
        "documents": [document]}), encoding="utf-8")
    return directory


def test_nodes_that_paint_nothing_leave_the_denominator() -> None:
    directory = _snapshot([
        {"tag": "DIV", "styles": {"background-color": "rgb(1, 2, 3)"}},
        {"tag": "DIV", "styles": {"background-color": "rgba(0, 0, 0, 0)"}},
        {"tag": "DIV", "styles": {}},
    ])
    report = audit.audit(directory, COMPAT, "t", "css")
    check(report["nodes_total"] == 3, "all three nodes are laid out")
    check(report["nodes_ink_bearing"] == 1,
          f"only the painted node counts, got {report['nodes_ink_bearing']}")


def test_svg_children_are_attributed_to_svg_not_to_style() -> None:
    """A <path> inside an <svg> has an ordinary CSS box and no CSS that draws it.

    Classified by style it returns drawable, which is how ~125 nodes of pooled
    SVG geometry were being counted as already handled.
    """
    directory = _snapshot([
        {"tag": "svg", "styles": {}, "box": [0, 0, 20, 20]},
        {"tag": "path", "parent": 0, "styles": {"background-color": "rgb(1,2,3)"},
         "box": [0, 0, 10, 10]},
        {"tag": "circle", "parent": 1, "styles": {"background-color": "rgb(1,2,3)"},
         "box": [0, 0, 5, 5]},
    ])
    report = audit.audit(directory, COMPAT, "t", "css")
    causes = report["causes"]
    check("svg-geometry" in causes, "svg geometry must have its own cause bucket")
    check(causes.get("svg-geometry", {}).get("nodes") == 3,
          f"the svg and both descendants attribute to svg, got {causes}")


def test_area_is_a_union_not_a_sum() -> None:
    """Nested boxes overlap; summing them can exceed the panel they sit in."""
    area = audit.union_area([(0, 0, 100, 100), (10, 10, 50, 50)], (100, 100))
    check(area == 10000, f"union of a box and its child is the outer box, got {area}")


def test_the_panel_is_the_declared_surface_not_the_scroll_extent() -> None:
    directory = _snapshot([{"tag": "DIV",
                            "styles": {"background-color": "rgb(1,2,3)"},
                            "box": [0, 0, 100, 100]}],
                          width=100, height=400)
    (directory / "capture.json").write_text(json.dumps({
        "provenance": {"viewport": {"resolved": {"width": 100, "height": 100}}}}),
        encoding="utf-8")
    report = audit.audit(directory, COMPAT, "t", "css")
    check(report["panel"] == {"width": 100, "height": 100},
          f"the declared viewport wins over contentHeight, got {report['panel']}")


def test_a_mismatched_style_row_is_refused_not_decoded() -> None:
    """Keying a style row by the wrong property is worse than not scoring it."""
    directory = _snapshot([{"tag": "DIV", "styles": {}}])
    path = directory / "dom-snapshot.json"
    snapshot = json.loads(path.read_text(encoding="utf-8"))
    snapshot["documents"][0]["layout"]["styles"][0].pop()
    path.write_text(json.dumps(snapshot), encoding="utf-8")
    try:
        audit.audit(directory, COMPAT, "t", "css")
    except SystemExit as exit_code:
        check(exit_code.code == audit.EX_INPUT,
              f"a short style row must exit EX_INPUT, got {exit_code.code}")
    else:
        FAILURES.append("a short style row was decoded instead of refused")


# ── the ledger's own invariant ──────────────────────────────────────────────

def test_every_form_names_a_declared_unsupported_value() -> None:
    compat = json.loads(COMPAT.read_text(encoding="utf-8"))
    for surface, section in compat.items():
        if not isinstance(section, dict) or surface.startswith("_"):
            continue
        for key, entry in section.items():
            if not isinstance(entry, dict):
                continue
            declared = set(entry.get("unsupportedValues") or [])
            for form in entry.get("unsupportedValueForms") or []:
                check(form.get("value") in declared,
                      f"{key}: form value is not in unsupportedValues — "
                      f"the machine and human lists have drifted")
                check(isinstance(form.get("match"), str) and form["match"],
                      f"{key}: form has no match expression")


def main() -> int:
    for name, function in sorted(globals().items()):
        if name.startswith("test_") and callable(function):
            function()
    if FAILURES:
        for failure in FAILURES:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("audit_paint_capability: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
