#!/usr/bin/env python3
"""Unit tests for check_panel_presence.py.

Every check here is proved in BOTH directions on the same fixture: the render
is first the reference itself, where the check must report nothing, and then
the same render with one defect painted into it, where that check must report
it. A check only ever watched passing is not known to be able to fail, and the
five checks in this file exist precisely because several earlier instruments in
this area were structurally unable to fire and read as clean.

The fixture is built to be non-degenerate, because degenerate fixtures are how
those instruments passed review: every box is non-square, the marks inside them
are off-centre, one node is translucent rather than opaque so the occlusion
rule is exercised rather than assumed, there is more than one accent colour so
"an accent" cannot mean "the only colour", and the wrapped paragraph's three
lines are all different widths so a line counter cannot be right by symmetry.
"""

from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
SCRIPT = THIS_DIR / "check_panel_presence.py"

try:
    import numpy as np
    from PIL import Image
except ImportError:  # pragma: no cover - reported as a skip, never as a pass
    np = None
    Image = None

SPEC = importlib.util.spec_from_file_location("check_panel_presence", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)

DPR = 2
PANEL_W, PANEL_H = 200, 200

STYLE_NAMES = [
    "display", "visibility", "opacity", "background-color", "background-image",
    "border-top-color", "border-right-color", "border-bottom-color",
    "border-left-color", "border-top-width", "border-right-width",
    "border-bottom-width", "border-left-width", "box-shadow", "color",
    "mix-blend-mode", "overflow",
]

BG = (20, 24, 31)
CARD = (32, 39, 51)
GLYPH = (216, 222, 233)
TEAL = (22, 218, 194)
CHIP_TEXT = (10, 22, 22)
ORANGE = (255, 106, 61)
VIOLET = (170, 90, 255)
BODY = (200, 210, 220)

# layout index -> (tag, parent, x, y, w, h, background, text)
# Nothing here is square and nothing is centred in its parent.
NODES = [
    ("DIV", -1, 0, 0, 200, 200, BG, ""),            # 0 root
    ("DIV", 0, 12, 10, 120, 54, CARD, ""),          # 1 card, 120x54
    ("DIV", 1, 22, 18, 16, 12, None, ""),           # 2 icon box, mark inset
    ("DIV", 1, 46, 18, 60, 20, TEAL, ""),           # 3 accent chip, 60x20
    ("#text", 3, 52, 23, 34, 10, None, "READY"),    # 4 chip label
    ("DIV", 0, 12, 70, 120, 16, None, ""),          # 5 blank strip, 120x16
    ("DIV", 0, 14, 92, 110, 40, None, ""),          # 6 paragraph container
    ("#text", 6, 14, 94, 96, 34, None, "wrapped"),  # 7 the wrapped run
    ("DIV", 0, 140, 10, 50, 70, None, ""),          # 8 translucent wash
    ("DIV", 0, 150, 112, 12, 8, ORANGE, ""),        # 9 second accent, 12x8
    # A neighbouring run in the SAME colour, close enough to sit inside the
    # wrapped run's measuring window. The line counter must ignore it because
    # it belongs to another node; a counter that reads every mark in the window
    # sees a fourth line here and stops being able to measure the run at all.
    ("#text", 0, 14, 136, 40, 6, None, "decoy"),    # 10 same-colour neighbour
] + [
    # A third accent, scattered so that NO single node holds enough of it to
    # reach the per-node floor. Only the panel-wide half of check 3 can see it
    # vanish; without it that half is never exercised and a mutation that
    # disables it passes the suite.
    ("DIV", 0, 8 + 30 * i, 148, 10, 6, None, "") for i in range(6)
] + [
    ("DIV", 0, 14, 160, 60, 26, None, ""),          # 17 second run's container
    ("#text", 17, 14, 162, 50, 22, None, "unclear"),  # 18 run the counter can't read
    ("DIV", 0, 150, 162, 10, 8, None, ""),          # 19 hidden under the cover
    ("DIV", 0, 146, 158, 24, 18, CARD, ""),         # 20 opaque cover, paints later
    # Sits just outside the wrapped run's container, in the run's own colour.
    # Whatever this node paints is ITS business; charging it to the run is how
    # a tab strip two px out of place once read as a caption overflowing a card
    # eighty px below it.
    ("DIV", 0, 128, 100, 14, 10, None, ""),         # 21 neighbour past the edge
    # A run whose own parent hugs it exactly. A container has to have room to
    # spare to be a constraint at all -- a box that already fits the glyphs
    # perfectly turns every difference in font metrics into an "overflow".
    ("DIV", 0, 140, 84, 40, 12, None, ""),          # 22 parent with no slack
    ("#text", 22, 140, 84, 40, 12, None, "tight"),  # 23 the run inside it
    # Too small to say anything about. A mark or two of punctuation cannot wrap
    # and cannot meaningfully overflow, and measuring it only invites whatever
    # else is nearby into the window.
    ("#text", 0, 190, 92, 4, 6, None, "."),         # 24 punctuation-sized run
]
ICON, CHIP, CHIP_LABEL, BLANK, PARA, RUN, WASH, DOT = 2, 3, 4, 5, 6, 7, 8, 9
DECOY = 10
SPARKS = list(range(11, 17))
SPARK_MARK = (4, 3)          # CSS px -> 8x6 device, under the per-node floor
RUN2, HIDDEN, COVER, NEIGHBOUR = 18, 19, 20, 21
TIGHT_BOX, TIGHT = 22, 23
TIGHT_LINE = (140.0, 84.0, 40.0, 12.0)
SPECK = 24
SPECK_LINE = (190.0, 92.0, 4.0, 6.0)

# Chrome says this run wraps to two lines; the reference only ever paints one.
# The line counter must notice that it cannot reproduce the declared count and
# decline, rather than quietly adopt its own reading as the truth.
RUN2_LINES = [(14.0, 162.0, 50.0, 10.0), (14.0, 174.0, 30.0, 10.0)]

# Three lines, three different widths, so no line count is right by symmetry.
RUN_LINES = [
    (14.0, 94.0, 96.0, 10.0),
    (14.0, 106.0, 72.0, 10.0),
    (14.0, 118.0, 41.0, 10.0),
]


def _style_row(strings, bg, colour, opacity="1"):
    def s(v):
        if v not in strings:
            strings.append(v)
        return strings.index(v)

    vals = {
        "display": "block", "visibility": "visible", "opacity": opacity,
        "background-color": bg or "rgba(0, 0, 0, 0)",
        "background-image": "none",
        "border-top-color": "rgba(0, 0, 0, 0)",
        "border-right-color": "rgba(0, 0, 0, 0)",
        "border-bottom-color": "rgba(0, 0, 0, 0)",
        "border-left-color": "rgba(0, 0, 0, 0)",
        "border-top-width": "0px", "border-right-width": "0px",
        "border-bottom-width": "0px", "border-left-width": "0px",
        "box-shadow": "none", "color": colour, "mix-blend-mode": "normal",
        "overflow": "visible",
    }
    return [s(vals[n]) for n in STYLE_NAMES]


def build_snapshot():
    strings = [""]

    def s(v):
        if v not in strings:
            strings.append(v)
        return strings.index(v)

    layout = {"nodeIndex": [], "styles": [], "bounds": [], "text": [],
              "paintOrders": []}
    nodes = {"parentIndex": [], "nodeType": [], "nodeName": [],
             "backendNodeId": []}
    for i, (tag, parent, x, y, w, h, bg, text) in enumerate(NODES):
        nodes["parentIndex"].append(parent)
        nodes["nodeType"].append(3 if tag == "#text" else 1)
        nodes["nodeName"].append(s(tag))
        nodes["backendNodeId"].append(1000 + i)
        colour = (f"rgb({CHIP_TEXT[0]}, {CHIP_TEXT[1]}, {CHIP_TEXT[2]})"
                  if i == CHIP_LABEL else
                  f"rgb({BODY[0]}, {BODY[1]}, {BODY[2]})")
        css_bg = (f"rgb({bg[0]}, {bg[1]}, {bg[2]})" if bg else None)
        # The wash is translucent on purpose: it must NOT be treated as an
        # opaque cover, or every node beneath it disappears from the checks.
        opacity = "1"
        if i == WASH:
            css_bg = "rgba(255, 255, 255, 0.06)"
        layout["nodeIndex"].append(i)
        layout["styles"].append(_style_row(strings, css_bg, colour, opacity))
        layout["bounds"].append([x, y, w, h])
        layout["text"].append(s(text) if text else s(""))
        layout["paintOrders"].append(i)

    text_boxes = {"layoutIndex": [], "bounds": [], "start": [], "length": []}
    start = 0
    for (bx, by, bw, bh) in RUN_LINES:
        text_boxes["layoutIndex"].append(RUN)
        text_boxes["bounds"].append([bx, by, bw, bh])
        text_boxes["start"].append(start)
        text_boxes["length"].append(8)
        start += 8
    for li, line in ((TIGHT, TIGHT_LINE), (SPECK, SPECK_LINE)):
        text_boxes["layoutIndex"].append(li)
        text_boxes["bounds"].append(list(line))
        text_boxes["start"].append(0)
        text_boxes["length"].append(5)
    start = 0
    for (bx, by, bw, bh) in RUN2_LINES:
        text_boxes["layoutIndex"].append(RUN2)
        text_boxes["bounds"].append([bx, by, bw, bh])
        text_boxes["start"].append(start)
        text_boxes["length"].append(6)
        start += 6

    return {
        "strings": strings,
        "computedStyleNames": STYLE_NAMES,
        "documents": [{"nodes": nodes, "layout": layout,
                       "textBoxes": text_boxes}],
    }


def _bars(img, x, y, w, h, colour, pitch=4, bar=2):
    """A row of vertical marks — a stand-in for glyphs with real structure."""
    x0, y0 = int(x * DPR), int(y * DPR)
    x1, y1 = int((x + w) * DPR), int((y + h) * DPR)
    for bx in range(x0, x1 - bar, pitch):
        img[y0:y1, bx:bx + bar] = colour


def build_reference():
    img = np.zeros((PANEL_H * DPR, PANEL_W * DPR, 3), np.uint8)
    for i, (_tag, _p, x, y, w, h, bg, _t) in enumerate(NODES):
        if bg is None:
            continue
        img[int(y * DPR):int((y + h) * DPR), int(x * DPR):int((x + w) * DPR)] = bg
    # The icon's mark is inset and off-centre inside its box, so the box's modal
    # colour is the card behind it and the mark reads as ink rather than fill.
    img[int(20 * DPR):int(27 * DPR), int(24 * DPR):int(31 * DPR)] = GLYPH
    _bars(img, 53, 24, 32, 8, CHIP_TEXT, pitch=5, bar=2)
    for i in SPARKS:
        sx, sy = NODES[i][2] + 2, NODES[i][3] + 1
        img[int(sy * DPR):int((sy + SPARK_MARK[1]) * DPR),
            int(sx * DPR):int((sx + SPARK_MARK[0]) * DPR)] = VIOLET
    for (bx, by, bw, bh) in RUN_LINES:
        _bars(img, bx, by + 1, bw, bh - 3, BODY, pitch=4, bar=2)
    bx, by, bw, bh = RUN2_LINES[0]
    _bars(img, bx, by + 1, bw, bh - 3, BODY, pitch=4, bar=2)
    dx, dy, dw, dh = NODES[DECOY][2:6]
    _bars(img, dx, dy + 1, dw, dh - 2, BODY, pitch=4, bar=2)
    tx, ty, tw, th = TIGHT_LINE
    _bars(img, tx, ty + 1, tw, th - 2, BODY, pitch=4, bar=2)
    sx, sy, sw, sh = SPECK_LINE
    _bars(img, sx, sy + 1, sw, sh - 2, BODY, pitch=3, bar=2)
    # A faint sliver between two of the wrapped run's lines, far below the ink
    # a real line carries. A line counter with a fixed floor reads it as a
    # fourth line and loses the ability to measure the run at all.
    img[232:234, 28:34:2] = BODY
    # Translucent wash, composited rather than replacing what is under it.
    wx, wy, ww, wh = NODES[WASH][2:6]
    sl = (slice(int(wy * DPR), int((wy + wh) * DPR)),
          slice(int(wx * DPR), int((wx + ww) * DPR)))
    img[sl] = (img[sl].astype(np.float32) * 0.94 + 255 * 0.06).astype(np.uint8)
    return img


class PresenceGateFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if np is None or Image is None:
            raise unittest.SkipTest("numpy and Pillow required")
        cls.tmp = Path(tempfile.mkdtemp(prefix="pulp-presence-"))
        cls.cap = cls.tmp / "capture"
        cls.cap.mkdir()
        cls.ref = build_reference()
        Image.fromarray(cls.ref).save(cls.cap / "browser.png")
        (cls.cap / "dom-snapshot.json").write_text(json.dumps(build_snapshot()))
        (cls.cap / "capture.json").write_text(json.dumps({
            "reference": {"logical_width": PANEL_W, "logical_height": PANEL_H,
                          "device_scale_factor": DPR}}))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # -- helpers ----------------------------------------------------------
    def run_gate(self, img, name, extra=()):
        png = self.tmp / f"{name}.png"
        out = self.tmp / f"{name}.json"
        Image.fromarray(img).save(png)
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "--capture", str(self.cap),
             "--render", str(png), "--label", name, "--json-out", str(out),
             *extra],
            capture_output=True, text=True, check=False)
        return proc, json.loads(out.read_text())

    def counts(self, doc):
        return {c: doc["checks"][c]["count"] for c in gate.CHECKS}

    def assert_only(self, name, img, check):
        """The seeded check fires; the control it was seeded from did not."""
        _proc, doc = self.run_gate(img, name)
        got = self.counts(doc)
        self.assertGreater(got[check], 0,
                           f"{name}: {check} stayed green on a seeded defect: {got}")
        return doc


class TestControlIsGreen(PresenceGateFixture):
    def test_reference_against_itself_reports_nothing(self):
        proc, doc = self.run_gate(self.ref, "control")
        self.assertEqual(proc.returncode, gate.EX_OK, proc.stderr)
        self.assertEqual(self.counts(doc),
                         {c: 0 for c in gate.CHECKS}, doc["checks"])

    def test_every_check_actually_ran(self):
        """Zero findings must mean "looked and found nothing", not "skipped"."""
        _proc, doc = self.run_gate(self.ref, "control-considered")
        for c in gate.CHECKS:
            self.assertGreater(doc["checks"][c]["considered"], 0,
                               f"{c} considered nothing, so its green is empty")

    def test_translucent_wash_does_not_occlude(self):
        """A node under the wash still owns its pixels and is still checked."""
        _proc, doc = self.run_gate(self.ref, "control-wash")
        self.assertNotIn(DOT, doc["unmeasurable_indices"],
                         "the node beneath the wash was skipped, not checked")
        img = self.ref.copy()
        x, y, w, h = NODES[DOT][2:6]
        img[int(y * DPR):int((y + h) * DPR),
            int(x * DPR):int((x + w) * DPR)] = CARD
        doc2 = self.assert_only("wash-dot-recoloured", img, "colour_present")
        self.assertTrue(any(f.get("layout_index") == DOT
                            for f in doc2["checks"]["colour_present"]["findings"]),
                        "a node beneath the translucent wash went unreported")


class TestSeededDefects(PresenceGateFixture):
    def test_1_deleted_ink_fires_ink_present(self):
        img = self.ref.copy()
        img[int(20 * DPR):int(27 * DPR), int(24 * DPR):int(31 * DPR)] = CARD
        doc = self.assert_only("seed-ink-deleted", img, "ink_present")
        hit = [f for f in doc["checks"]["ink_present"]["findings"]
               if f["layout_index"] == ICON]
        self.assertTrue(hit, "the node whose mark was deleted was not named")
        self.assertEqual(hit[0]["render_ink_px"], 0)

    def test_2_invented_border_fires_ink_absent(self):
        img = self.ref.copy()
        x, y, w, h = NODES[BLANK][2:6]
        x0, y0 = int(x * DPR), int(y * DPR)
        x1, y1 = int((x + w) * DPR), int((y + h) * DPR)
        for t in range(2):
            img[y0 + t, x0:x1] = GLYPH
            img[y1 - 1 - t, x0:x1] = GLYPH
            img[y0:y1, x0 + t] = GLYPH
            img[y0:y1, x1 - 1 - t] = GLYPH
        doc = self.assert_only("seed-border-invented", img, "ink_absent")
        self.assertTrue(any(f["layout_index"] == BLANK
                            for f in doc["checks"]["ink_absent"]["findings"]),
                        "the node given a border it never had was not named")

    def test_3_stripped_accent_fires_colour_present(self):
        img = self.ref.copy()
        x, y, w, h = NODES[CHIP][2:6]
        sl = (slice(int(y * DPR), int((y + h) * DPR)),
              slice(int(x * DPR), int((x + w) * DPR)))
        block = img[sl]
        teal = np.all(block == TEAL, axis=-1)
        block[teal] = (120, 120, 120)
        img[sl] = block
        doc = self.assert_only("seed-accent-stripped", img, "colour_present")
        rgbs = [tuple(f["rgb"]) for f in doc["checks"]["colour_present"]["findings"]]
        self.assertIn(TEAL, rgbs, f"the stripped accent was not named: {rgbs}")

    def test_3b_other_accent_alone_is_not_enough(self):
        """A surviving accent must not vouch for a vanished one."""
        img = self.ref.copy()
        x, y, w, h = NODES[DOT][2:6]
        img[int(y * DPR):int((y + h) * DPR),
            int(x * DPR):int((x + w) * DPR)] = CARD
        doc = self.assert_only("seed-dot-removed", img, "colour_present")
        rgbs = [tuple(f["rgb"]) for f in doc["checks"]["colour_present"]["findings"]]
        self.assertIn(ORANGE, rgbs,
                      f"the smaller accent vanished unreported: {rgbs}")

    def test_3c_accent_below_the_per_node_floor_is_caught_panel_wide(self):
        """No node holds enough of it to be asked; the panel still must be."""
        img = self.ref.copy()
        spark = np.all(img == VIOLET, axis=-1)
        self.assertGreater(int(spark.sum()), 0, "fixture painted no third accent")
        img[spark] = (120, 120, 120)
        doc = self.assert_only("seed-scattered-accent", img, "colour_present")
        hit = [f for f in doc["checks"]["colour_present"]["findings"]
               if f["scope"] == "panel" and tuple(f["rgb"]) == VIOLET]
        self.assertTrue(hit, "a scattered accent vanished with no panel-wide "
                             f"report: {doc['checks']['colour_present']['findings']}")

    def test_4_overflowing_run_fires_text_contained(self):
        img = self.ref.copy()
        # The run's own container ends at x=124; put its marks past that.
        bx, by, bw, bh = RUN_LINES[0]
        _bars(img, 128, by + 1, 40, bh - 3, BODY, pitch=4, bar=2)
        doc = self.assert_only("seed-text-overflow", img, "text_contained")
        hit = [f for f in doc["checks"]["text_contained"]["findings"]
               if f["layout_index"] == RUN and f["side"] == "right"]
        self.assertTrue(hit, "the run that ran past its container was not named")
        self.assertEqual(hit[0]["mode"], "overflow")

    def test_4b_run_pinned_to_its_container_edge_fires_as_clipped(self):
        """Losing the end of a word to a clip leaves no ink outside to find.

        What is left is the run reaching exactly as far as the box allows,
        where the reference stopped well short of it -- so that, not spill, is
        what has to be recognised.
        """
        img = self.ref.copy()
        _bx, by, _bw, bh = RUN_LINES[0]
        cont_right = NODES[PARA][2] + NODES[PARA][4]
        run_right = RUN_LINES[0][0] + RUN_LINES[0][2]
        self.assertGreater(cont_right - run_right, 8,
                           "fixture leaves no room to be clipped in")
        x0 = int(run_right * DPR)
        x1 = int(cont_right * DPR) - 1            # right up to the edge, not past
        for bx in range(x0, x1 - 2, 4):
            img[int((by + 1) * DPR):int((by + bh - 2) * DPR), bx:bx + 2] = BODY
        doc = self.assert_only("seed-text-clipped", img, "text_contained")
        hit = [f for f in doc["checks"]["text_contained"]["findings"]
               if f["layout_index"] == RUN and f["side"] == "right"]
        self.assertTrue(hit, "the run pinned to its container edge was not named")
        self.assertEqual(hit[0]["mode"], "clipped", hit[0])

    def test_4c_a_container_with_no_room_is_not_a_constraint(self):
        """A run inside a box that already hugs it cannot be said to overflow."""
        img = self.ref.copy()
        tx, ty, tw, th = TIGHT_LINE
        _bars(img, tx + tw, ty + 1, 6.0, th - 2, BODY, pitch=4, bar=2)
        _proc, doc = self.run_gate(img, "tight-parent-widened")
        self.assertFalse([f for f in doc["checks"]["text_contained"]["findings"]
                          if f["layout_index"] == TIGHT],
                         "a run a few px wider than the box that hugs it was "
                         "reported as overflowing")

    def test_4d_a_run_too_small_to_read_is_declined_not_measured(self):
        _proc, doc = self.run_gate(self.ref, "speck-control")
        reasons = [u["reason"] for u in doc["unmeasurable"]
                   if u.get("layout_index") == SPECK]
        self.assertTrue(any("too small" in r for r in reasons),
                        f"a punctuation-sized run was measured anyway: {reasons}")

    def test_5_collapsed_wrap_fires_text_runs(self):
        img = self.ref.copy()
        for (bx, by, bw, bh) in RUN_LINES[1:]:
            img[int(by * DPR):int((by + bh) * DPR),
                int(bx * DPR):int((bx + bw) * DPR)] = BG
        doc = self.assert_only("seed-lines-collapsed", img, "text_runs")
        hit = [f for f in doc["checks"]["text_runs"]["findings"]
               if f["layout_index"] == RUN]
        self.assertTrue(hit, "the collapsed run was not named")
        self.assertEqual(hit[0]["reference_lines"], len(RUN_LINES))
        self.assertLess(hit[0]["render_lines"], len(RUN_LINES))


class TestRefusesToGuess(PresenceGateFixture):
    """Where the instrument cannot read the reference, it must say so."""

    def test_line_counter_declines_a_run_it_cannot_reproduce(self):
        _proc, doc = self.run_gate(self.ref, "unreadable-control")
        self.assertIn(RUN2, doc["unmeasurable_indices"],
                      "the counter adopted its own reading of a run whose "
                      "declared line count it cannot reproduce")
        reasons = [u for u in doc["unmeasurable"]
                   if u.get("layout_index") == RUN2
                   and u.get("check") == "text_runs"]
        self.assertTrue(reasons, doc["unmeasurable"])

        # A render that paints BOTH declared lines differs from the reference,
        # which paints one. Reporting that as a wrap regression would be
        # reporting the reference's own unreadability as our defect.
        img = self.ref.copy()
        bx, by, bw, bh = RUN2_LINES[1]
        _bars(img, bx, by + 1, bw, bh - 3, BODY, pitch=4, bar=2)
        _proc, doc = self.run_gate(img, "unreadable-render")
        self.assertFalse([f for f in doc["checks"]["text_runs"]["findings"]
                          if f["layout_index"] == RUN2],
                         "a run the counter declined to measure was reported anyway")

    def test_a_neighbours_marks_are_not_charged_to_the_run(self):
        """Containment asks about the run's ink, not about everything nearby."""
        img = self.ref.copy()
        x, y, w, h = NODES[NEIGHBOUR][2:6]
        _bars(img, x + 1, y + 1, w - 2, h - 2, BODY, pitch=4, bar=2)
        _proc, doc = self.run_gate(img, "neighbour-past-the-edge")
        self.assertFalse([f for f in doc["checks"]["text_contained"]["findings"]
                          if f["layout_index"] == RUN],
                         "another node's marks were reported as the run "
                         "overflowing its container")
        self.assertTrue([f for f in doc["checks"]["ink_absent"]["findings"]
                         if f["layout_index"] == NEIGHBOUR],
                        "the node that actually gained marks went unreported")

    def test_a_mark_under_an_opaque_cover_is_charged_to_the_cover(self):
        """Occlusion decides ownership before box size does.

        The hidden node has the smaller box, so specificity alone would hand it
        every pixel inside the cover -- pixels it cannot possibly be painting,
        because an opaque node sits on top of it. A mark appearing there is the
        cover's to answer for.
        """
        img = self.ref.copy()
        x, y, w, h = NODES[HIDDEN][2:6]
        img[int((y + 1) * DPR):int((y + h - 1) * DPR),
            int((x + 1) * DPR):int((x + w - 1) * DPR)] = GLYPH
        doc = self.assert_only("seed-mark-under-cover", img, "ink_absent")
        named = {f["layout_index"] for f in doc["checks"]["ink_absent"]["findings"]}
        self.assertIn(COVER, named,
                      f"the opaque cover was not charged for the mark: {named}")
        self.assertNotIn(HIDDEN, named,
                         "a node buried under an opaque fill was charged for a "
                         "mark it cannot have painted")


class TestGating(PresenceGateFixture):
    def test_blank_render_is_refused_before_any_per_node_verdict(self):
        """A blank render matches a dark reference on any area metric."""
        img = np.zeros_like(self.ref)
        img[:, :] = BG
        proc, doc = self.run_gate(img, "blank")
        self.assertEqual(proc.returncode, gate.EX_FINDING, proc.stderr)
        self.assertFalse(doc["global_ink"]["ok"])
        self.assertIn("substantially blank", proc.stderr)

    def test_absolute_cap_fires_and_clean_run_does_not(self):
        img = self.ref.copy()
        img[int(20 * DPR):int(27 * DPR), int(24 * DPR):int(31 * DPR)] = CARD
        proc, _doc = self.run_gate(img, "cap-red", ["--max-ink-present", "0"])
        self.assertEqual(proc.returncode, gate.EX_FINDING, proc.stderr)
        proc, _doc = self.run_gate(self.ref, "cap-green",
                                   ["--max-ink-present", "0"])
        self.assertEqual(proc.returncode, gate.EX_OK, proc.stderr)

    def test_baseline_carries_known_findings_and_gates_only_new_ones(self):
        img = self.ref.copy()
        img[int(20 * DPR):int(27 * DPR), int(24 * DPR):int(31 * DPR)] = CARD
        _proc, _doc = self.run_gate(img, "base-first")
        base = self.tmp / "base-first.json"

        # Same defect, checked against a baseline that already holds it.
        proc, doc = self.run_gate(img, "base-same",
                                  ["--baseline", str(base)])
        self.assertEqual(proc.returncode, gate.EX_OK, proc.stderr)
        self.assertEqual(doc["regression"]["new_count"], 0)

        # One more defect on top: only the new one is gated.
        worse = img.copy()
        for (bx, by, bw, bh) in RUN_LINES[1:]:
            worse[int(by * DPR):int((by + bh) * DPR),
                  int(bx * DPR):int((bx + bw) * DPR)] = BG
        proc, doc = self.run_gate(worse, "base-worse",
                                  ["--baseline", str(base)])
        self.assertEqual(proc.returncode, gate.EX_FINDING, proc.stderr)
        self.assertGreater(doc["regression"]["new_count"], 0)
        self.assertTrue(any(k.startswith("text_runs:")
                            for k in doc["regression"]["new"]),
                        doc["regression"]["new"])

    def test_size_mismatch_is_refused_rather_than_resized(self):
        img = self.ref[:, : self.ref.shape[1] - 8]
        png = self.tmp / "short.png"
        Image.fromarray(img).save(png)
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "--capture", str(self.cap),
             "--render", str(png)], capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, gate.EX_SIZE, proc.stderr)


class TestUnits(unittest.TestCase):
    def test_parse_rgb_rejects_what_it_cannot_read(self):
        self.assertEqual(gate.parse_rgb("rgb(1, 2, 3)"), (1, 2, 3))
        self.assertEqual(gate.parse_rgb("rgba(1, 2, 3, 0.5)"), (1, 2, 3))
        self.assertIsNone(gate.parse_rgb("rgba(1, 2, 3, 0)"))
        self.assertIsNone(gate.parse_rgb("color(display-p3 1 0 0)"))
        self.assertIsNone(gate.parse_rgb(""))

    def test_band_and_extent_agree_with_a_hand_counted_profile(self):
        if np is None:
            self.skipTest("numpy required")
        p = np.array([0, 5, 6, 0, 0, 4, 0, 9, 9, 0])
        self.assertEqual(gate._bands(p, 4), 3)
        self.assertEqual(gate._bands(p, 5), 2)
        self.assertEqual(gate._extent(p, 4), (1, 8))
        self.assertIsNone(gate._extent(p, 20))

    def test_finding_key_separates_nodes_scopes_and_sides(self):
        panel = gate.finding_key("colour_present",
                                 {"scope": "panel", "rgb": [1, 2, 3]})
        node = gate.finding_key("colour_present",
                                {"scope": "node", "layout_index": 7,
                                 "rgb": [1, 2, 3]})
        self.assertNotEqual(panel, node)
        left = gate.finding_key("text_contained",
                                {"layout_index": 7, "side": "left",
                                 "mode": "overflow"})
        right = gate.finding_key("text_contained",
                                 {"layout_index": 7, "side": "right",
                                  "mode": "overflow"})
        self.assertNotEqual(left, right)


if __name__ == "__main__":
    if np is None or Image is None:
        # ctest reads 77 as SKIPPED. Running only the handful of dependency-free
        # unit tests and exiting 0 would report this file as PASSED on an
        # interpreter that cannot execute a single seeded-defect proof, which is
        # the precise shape of failure this whole file exists to rule out.
        print("SKIP: numpy and Pillow are required to run the presence-gate "
              "proofs; none of them ran.", file=sys.stderr)
        sys.exit(77)
    unittest.main()
