#!/usr/bin/env python3
"""Tests for tools/import-design/layout_parity.py.

The tool's value is not "it computes a subtraction" — it is that a real layout
bug produces ONE readable finding that names the node to look at. So the tests
concentrate on the two behaviours that deliver that, and which a naive
per-node diff gets wrong:

* **parent-relative attribution** — a misplaced container drags every descendant
  with it. Reporting all of them buries the one node that broke, so a node's
  delta has its parent's subtracted out and only the origin of the error is
  reported.
* **clustering** — one dropped alignment moves every child of one frame. The
  group collapses to a single finding naming the parent, and the shared-offset
  case (the CENTER/MAX signature) is called out by name.

Plus a PASS control. A checker that can only ever report failure is
indistinguishable from a broken one, so "it caught the bug" means nothing
without proving it stays silent on a good import.

Pure stdlib — no image IO, no optional dependency, nothing to skip for.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = (pathlib.Path(__file__).resolve().parent.parent
        / "tools" / "import-design" / "layout_parity.py")

_spec = importlib.util.spec_from_file_location("layout_parity", TOOL)
layout_parity = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(layout_parity)


def geometry(*nodes: dict) -> dict:
    """A design-side document. Each node: (id, parent, x, y, w, h[, name])."""
    return {"units": "design-px", "nodes": list(nodes)}


def node(node_id: str, parent_id, x: float, y: float,
         w: float = 10, h: float = 10, name: str | None = None) -> dict:
    return {"node_id": node_id, "parent_id": parent_id,
            "name": name or f"node-{node_id}", "type": "FRAME",
            "x": x, "y": y, "width": w, "height": h}


def view(node_id: str, x: float, y: float, w: float = 10, h: float = 10) -> dict:
    return {"anchor_id": f"figma-plugin:{node_id}", "node_id": node_id,
            "type": "View", "visible": True,
            "x": x, "y": y, "width": w, "height": h}


def layout(*views: dict) -> dict:
    return {"units": "design-px", "views": list(views)}


class ParityTestCase(unittest.TestCase):
    def compare(self, geom: dict, lay: dict, tol: float = 2.0) -> dict:
        with tempfile.TemporaryDirectory() as td:
            gp = pathlib.Path(td) / "g.json"
            lp = pathlib.Path(td) / "l.json"
            gp.write_text(json.dumps(geom))
            lp.write_text(json.dumps(lay))
            return layout_parity.compare(gp, lp, tol)


class TestPassControl(ParityTestCase):
    """Prove the tool can report success before trusting it to report failure."""

    def test_matching_layout_reports_no_findings(self):
        geom = geometry(node("0:1", None, 0, 0, 100, 100),
                        node("0:2", "0:1", 10, 10),
                        node("0:3", "0:1", 40, 10))
        lay = layout(view("0:1", 0, 0, 100, 100), view("0:2", 10, 10), view("0:3", 40, 10))
        report = self.compare(geom, lay)
        self.assertEqual(report["findings"], [])
        self.assertEqual(report["dropped"], [])
        self.assertEqual(report["extra"], [])
        self.assertEqual(report["matched"], 3)
        self.assertIn("PASS", layout_parity.render_report(report, 25))

    def test_sub_tolerance_rounding_is_not_a_finding(self):
        # styleFor rounds via Math.round and Yoga snaps to the pixel grid; a 1px
        # disagreement is arithmetic. A tool that flags it cries wolf and gets
        # ignored, which is the failure mode that matters here.
        geom = geometry(node("0:1", None, 0, 0, 100, 100), node("0:2", "0:1", 10.4, 10.4))
        lay = layout(view("0:1", 0, 0, 100, 100), view("0:2", 11, 11))
        self.assertEqual(self.compare(geom, lay)["findings"], [])


class TestClustering(ParityTestCase):
    def test_shared_offset_collapses_to_one_finding_naming_the_parent(self):
        # The CENTER/MAX signature: the parent is placed correctly, and dropping
        # its alignment moves every child by the SAME amount. Eight findings
        # would bury it; one naming the parent is the whole point.
        geom = geometry(
            node("0:1", None, 0, 0, 500, 100, name="Frame"),
            node("0:2", "0:1", 214, 10, name="MiniSeqLane"),
            *[node(f"0:{10 + i}", "0:2", 214 + i * 20, 10, name=f"step{i}") for i in range(8)],
        )
        lay = layout(
            view("0:1", 0, 0, 500, 100), view("0:2", 214, 10),
            *[view(f"0:{10 + i}", i * 20, 10) for i in range(8)],
        )
        report = self.compare(geom, lay)
        self.assertEqual(len(report["findings"]), 1, "eight children, one finding")
        finding = report["findings"][0]
        self.assertEqual(finding["kind"], "cluster")
        self.assertEqual(finding["parent_id"], "0:2")
        self.assertTrue(finding["shared_offset"])
        self.assertEqual(finding["child_count"], 8)
        self.assertIn("MiniSeqLane", finding["message"])
        self.assertIn("dx=-214", finding["message"])
        self.assertIn("alignment/padding dropped", finding["message"])

    def test_divergent_deltas_cluster_but_are_not_called_a_shared_offset(self):
        # The dropped-flex signature: children pile at the parent's origin, each
        # off by however far it should have flowed. Still one parent to blame,
        # but calling this a shared offset would name the wrong cause.
        geom = geometry(
            node("0:1", None, 0, 0, 500, 100, name="Root"),
            node("0:2", "0:1", 0, 0, name="Transport"),
            node("0:3", "0:2", 0, 0), node("0:4", "0:2", 38, 0), node("0:5", "0:2", 76, 0),
        )
        lay = layout(view("0:1", 0, 0, 500, 100), view("0:2", 0, 0),
                     view("0:3", 0, 0), view("0:4", 0, 0), view("0:5", 0, 0))
        report = self.compare(geom, lay)
        self.assertEqual(len(report["findings"]), 1)
        finding = report["findings"][0]
        self.assertEqual(finding["kind"], "cluster")
        self.assertFalse(finding["shared_offset"])
        self.assertIn("Transport", finding["message"])
        self.assertIn("layout contract", finding["message"])
        # The per-child deltas stay available — the cluster summarises, it does
        # not discard the evidence.
        self.assertEqual(sorted(c["dx"] for c in finding["children"]), [-76, -38])

    def test_a_lone_offender_is_reported_as_itself_not_as_its_parent(self):
        # Clustering a single child under a parent that placed every other child
        # correctly would point at the wrong node.
        geom = geometry(node("0:1", None, 0, 0, 500, 100, name="Root"),
                        node("0:2", "0:1", 10, 10, name="ok"),
                        node("0:3", "0:1", 40, 10, name="broken"))
        lay = layout(view("0:1", 0, 0, 500, 100), view("0:2", 10, 10), view("0:3", 90, 10))
        report = self.compare(geom, lay)
        self.assertEqual(len(report["findings"]), 1)
        self.assertEqual(report["findings"][0]["kind"], "node")
        self.assertEqual(report["findings"][0]["node_id"], "0:3")
        self.assertIn("broken", report["findings"][0]["message"])

    def test_findings_are_ordered_worst_first(self):
        # Separate parents, so each offender stays its own finding — two
        # offenders under ONE parent would (correctly) cluster into one.
        geom = geometry(node("0:1", None, 0, 0, 500, 500, name="Root"),
                        node("0:2", "0:1", 0, 0, 200, 200, name="boxA"),
                        node("0:3", "0:1", 0, 200, 200, 200, name="boxB"),
                        node("0:4", "0:2", 0, 0, name="small"),
                        node("0:5", "0:3", 0, 200, name="huge"))
        lay = layout(view("0:1", 0, 0, 500, 500),
                     view("0:2", 0, 0, 200, 200), view("0:3", 0, 200, 200, 200),
                     view("0:4", 5, 0), view("0:5", 200, 200))
        report = self.compare(geom, lay)
        self.assertEqual([f["node_id"] for f in report["findings"]], ["0:5", "0:4"])


class TestParentRelativeAttribution(ParityTestCase):
    def test_a_shifted_container_reports_once_not_once_per_descendant(self):
        # A panel 214px off drags its 50 children with it. Each child is placed
        # perfectly WITHIN the panel, so the panel is the only bug — and a report
        # with 51 lines in it is a report nobody reads.
        geom = geometry(node("0:1", None, 0, 0, 800, 400, name="Root"),
                        node("0:2", "0:1", 300, 0, name="Panel"),
                        *[node(f"1:{i}", "0:2", 300 + i, 0) for i in range(50)])
        lay = layout(view("0:1", 0, 0, 800, 400), view("0:2", 86, 0),
                     *[view(f"1:{i}", 86 + i, 0) for i in range(50)])
        report = self.compare(geom, lay)
        self.assertEqual(len(report["findings"]), 1, "one bug, one finding")
        self.assertEqual(report["findings"][0]["node_id"], "0:2")
        self.assertEqual(report["findings"][0]["dx"], -214)

    def test_a_child_misplaced_inside_a_shifted_parent_is_still_reported(self):
        # The counterpart bound: subtracting the parent's delta must not swallow
        # a genuine error of the child's own.
        geom = geometry(node("0:1", None, 0, 0, 800, 400, name="Root"),
                        node("0:2", "0:1", 300, 0, name="Panel"),
                        node("0:3", "0:2", 300, 0, name="Inner"))
        lay = layout(view("0:1", 0, 0, 800, 400), view("0:2", 86, 0), view("0:3", 120, 0))
        report = self.compare(geom, lay)
        by_id = {f["node_id"]: f for f in report["findings"]}
        self.assertEqual(sorted(by_id), ["0:2", "0:3"])
        self.assertEqual(by_id["0:2"]["dx"], -214, "the panel owns its own shift")
        self.assertEqual(by_id["0:3"]["dx"], 34, "the child owns only the remainder")

    def test_a_size_delta_is_never_parent_relative(self):
        # A parent being the wrong size does not make its child the wrong size;
        # inheriting the delta here would hide real size bugs.
        geom = geometry(node("0:1", None, 0, 0, 100, 100, name="Root"),
                        node("0:2", "0:1", 0, 0, 50, 50, name="Child"))
        lay = layout(view("0:1", 0, 0, 60, 100), view("0:2", 0, 0, 50, 50))
        report = self.compare(geom, lay)
        self.assertEqual(len(report["findings"]), 1)
        self.assertEqual(report["findings"][0]["node_id"], "0:1")
        self.assertEqual(report["findings"][0]["dw"], -40)


class TestStructuralCompleteness(ParityTestCase):
    def test_a_node_the_render_never_produced_is_reported_as_dropped(self):
        geom = geometry(node("0:1", None, 0, 0, 100, 100),
                        node("0:2", "0:1", 10, 10, name="vanished"))
        lay = layout(view("0:1", 0, 0, 100, 100))
        report = self.compare(geom, lay)
        self.assertEqual(len(report["dropped"]), 1)
        self.assertIn("vanished", report["dropped"][0]["label"])
        self.assertIn("DROPPED", layout_parity.render_report(report, 25))

    def test_a_view_with_no_counterpart_in_the_design_is_reported_as_extra(self):
        geom = geometry(node("0:1", None, 0, 0, 100, 100))
        lay = layout(view("0:1", 0, 0, 100, 100), view("0:9", 0, 0))
        report = self.compare(geom, lay)
        self.assertEqual(report["extra"], ["0:9"])

    def test_an_unanchored_view_is_ignored_rather_than_counted_as_extra(self):
        # Codegen emits scaffolding the design never named (wrappers, spacers).
        # It carries no node_id, so it joins to nothing and must not be noise.
        geom = geometry(node("0:1", None, 0, 0, 100, 100))
        lay = {"views": [view("0:1", 0, 0, 100, 100),
                         {"anchor_id": "", "type": "View", "x": 0, "y": 0,
                          "width": 1, "height": 1}]}
        report = self.compare(geom, lay)
        self.assertEqual(report["extra"], [])
        self.assertEqual(report["findings"], [])


class TestCli(unittest.TestCase):
    """The exit code IS the gate, so it gets tested through the real binary."""

    def run_tool(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(TOOL), *args],
                              capture_output=True, text=True)

    def write_pair(self, td: pathlib.Path, geom: dict, lay: dict) -> pathlib.Path:
        # Named the way --dump-layout names them, so the sibling default is
        # exercised rather than assumed.
        (td / "layout.geometry.json").write_text(json.dumps(geom))
        dump = td / "layout.json"
        dump.write_text(json.dumps(lay))
        return dump

    def test_exit_zero_on_parity_and_one_on_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            good = geometry(node("0:1", None, 0, 0, 100, 100), node("0:2", "0:1", 10, 10))
            dump = self.write_pair(td, good, layout(view("0:1", 0, 0, 100, 100),
                                                    view("0:2", 10, 10)))
            ok = self.run_tool(str(dump))
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            self.assertIn("PASS", ok.stdout)

            self.write_pair(td, good, layout(view("0:1", 0, 0, 100, 100), view("0:2", 99, 10)))
            bad = self.run_tool(str(dump))
            self.assertEqual(bad.returncode, 1)
            self.assertIn("MISPLACED", bad.stdout)

    def test_a_missing_geometry_sidecar_is_a_usage_error_with_a_hint(self):
        # Exit 2, not 0: a parity run that silently checks nothing is worse than
        # no parity run, because it reports success.
        with tempfile.TemporaryDirectory() as tmp:
            dump = pathlib.Path(tmp) / "layout.json"
            dump.write_text(json.dumps(layout(view("0:1", 0, 0))))
            result = self.run_tool(str(dump))
            self.assertEqual(result.returncode, 2)
            self.assertIn("--dump-layout", result.stderr)

    def test_json_report_is_written_and_is_not_capped_by_max_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            td = pathlib.Path(tmp)
            geom = geometry(node("0:1", None, 0, 0, 500, 500),
                            *[node(f"0:{i}", "0:1", i * 10, 0, name=f"n{i}") for i in range(2, 8)])
            dump = self.write_pair(td, geom, layout(
                view("0:1", 0, 0, 500, 500),
                *[view(f"0:{i}", i * 10 + i * 7, 0) for i in range(2, 8)]))
            out = td / "report.json"
            result = self.run_tool(str(dump), "--max-findings", "1", "--json", str(out))
            self.assertEqual(result.returncode, 1)
            report = json.loads(out.read_text())
            self.assertGreater(len(report["findings"]), 0)
            self.assertEqual(report["tolerance_px"], 2.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
