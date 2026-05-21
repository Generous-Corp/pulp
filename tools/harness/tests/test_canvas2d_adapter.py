"""Tests for the Canvas2D harness adapter classifier.

Invocation::

    python3 -m unittest tools.harness.tests.test_canvas2d_adapter
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry  # noqa: E402
from tools.harness.adapters.canvas2d import Canvas2dAdapter  # noqa: E402
from tools.harness.status import Status  # noqa: E402


def _entry(
    name: str,
    *,
    status: str = "supported",
    maps_to: str = "native route",
    notes: str | None = None,
    supported_values: list[str] | None = None,
    unsupported_values: list[str] | None = None,
) -> CatalogEntry:
    return CatalogEntry(
        surface="canvas2d",
        name=f"canvas2d/{name}",
        status=status,
        maps_to=maps_to,
        notes=notes,
        supported_values=list(supported_values or []),
        unsupported_values=list(unsupported_values or []),
    )


class Canvas2dAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)

        oracle_dir = self.repo / "tools" / "harness" / "oracles" / "canvas2d"
        oracle_dir.mkdir(parents=True)
        oracle = {
            "entries": {
                "fillRect": {
                    "kind": "method",
                    "bridge": ["canvasFillRect"],
                },
                "stroke": {
                    "kind": "method",
                    "bridge": ["canvasStroke"],
                },
                "arc": {
                    "kind": "method",
                    "bridge": ["canvasArc"],
                },
                "missingBridge": {
                    "kind": "method",
                    "bridge": ["canvasMissingBridge"],
                },
                "missingShim": {
                    "kind": "method",
                    "bridge": [],
                },
                "citesMissing": {
                    "kind": "method",
                    "bridge": ["canvasStroke"],
                },
                "createPattern": {
                    "kind": "method",
                    "expectedStatus": "missing",
                    "gotcha": "not implemented",
                    "bridge": [],
                },
                "transform": {
                    "kind": "method",
                    "expectedStatus": "partial",
                    "gotcha": "rotation ignored",
                    "bridge": ["canvasTransform"],
                },
                "lineCap": {
                    "kind": "attribute",
                    "bridge": ["canvasSetLineCap"],
                    "values": ["butt", "round", "square"],
                },
                "textAlign": {
                    "kind": "attribute",
                    "bridge": ["canvasSetTextAlign"],
                    "values": ["left", "right", "center"],
                },
                "lineJoin": {
                    "kind": "attribute",
                    "bridge": ["canvasSetLineJoin"],
                    "values": ["miter", "round", "bevel"],
                },
                "shadowBlur": {
                    "kind": "attribute",
                    "bridge": ["canvasSetShadowBlur"],
                },
                "globalAlpha": {
                    "kind": "attribute",
                    "bridge": ["canvasSetGlobalAlpha"],
                },
            }
        }
        (oracle_dir / "canvas2d-supported.json").write_text(json.dumps(oracle))

        js_dir = self.repo / "core" / "view" / "js"
        js_dir.mkdir(parents=True)
        (js_dir / "web-compat-canvas.js").write_text(
            "CanvasRenderingContext2D.prototype.fillRect = function() {};\n"
            "CanvasRenderingContext2D.prototype.stroke = function() {};\n"
            "CanvasRenderingContext2D.prototype.arc = function() {};\n"
            "CanvasRenderingContext2D.prototype.citesMissing = function() {};\n"
            "CanvasRenderingContext2D.prototype.transform = function() {};\n"
            "CanvasGradient.prototype.addColorStop = function() {};\n"
            "function CanvasRenderingContext2D() {\n"
            "  this.lineCap = 'butt';\n"
            "  this.textAlign = 'start';\n"
            "  this.lineJoin = 'miter';\n"
            "  this.shadowBlur = 0;\n"
            "}\n"
        )

        bridge_dir = self.repo / "core" / "view" / "src"
        bridge_dir.mkdir(parents=True)
        (bridge_dir / "widget_bridge.cpp").write_text(
            'engine_.register_function("canvasFillRect", []{});\n'
            'engine_.register_function("canvasStroke", []{});\n'
            'engine_.register_function("canvasArc", []{});\n'
            'engine_.register_function("canvasTransform", []{});\n'
            'engine_.register_function("canvasSetLineCap", []{});\n'
            'engine_.register_function("canvasSetTextAlign", []{});\n'
            'engine_.register_function("canvasSetLineJoin", []{});\n'
            'engine_.register_function("canvasSetShadowBlur", []{});\n'
            'engine_.register_function("canvasSetGlobalAlpha", []{});\n'
        )

        self.adapter = Canvas2dAdapter(self.repo)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_surface_extractors_find_bridge_methods_and_attributes(self) -> None:
        self.assertIn("canvasFillRect", self.adapter._bridge_fns)
        self.assertIn("fillRect", self.adapter._shim_methods)
        self.assertIn("addColorStop", self.adapter._shim_methods)
        self.assertIn("lineCap", self.adapter._shim_attrs)
        self.assertIn("shadowBlur", self.adapter._shim_attrs)
        self.assertNotIn("canvasMissingBridge", self.adapter._bridge_fns)

    def test_maps_notes_and_bridge_call_helpers(self) -> None:
        self.assertTrue(Canvas2dAdapter._maps_to_marks_unimpl(""))
        self.assertTrue(Canvas2dAdapter._maps_to_marks_unimpl("not implemented"))
        self.assertFalse(Canvas2dAdapter._maps_to_marks_unimpl("no bridge round-trip needed"))
        self.assertTrue(Canvas2dAdapter._maps_to_marks_noop("graceful fallback"))
        self.assertFalse(Canvas2dAdapter._maps_to_marks_noop(""))
        self.assertTrue(Canvas2dAdapter._notes_marks_unimpl("not pushed to bridge"))
        self.assertFalse(Canvas2dAdapter._notes_marks_unimpl(""))
        self.assertEqual(Canvas2dAdapter._bridge_calls_in_maps_to(""), [])
        self.assertCountEqual(
            Canvas2dAdapter._bridge_calls_in_maps_to(
                "calls canvasFillRect and canvasMissingBridge then canvasFillRect"
            ),
            ["canvasMissingBridge", "canvasFillRect"],
        )

    def test_wontfix_and_missing_oracle_entries_are_oos(self) -> None:
        wontfix = self.adapter.run(_entry("fillRect", status="wontfix"))
        self.assertIs(wontfix.status, Status.OOS)

        missing_oracle = self.adapter.run(_entry("_native_canvasFillCircle", status="missing"))
        self.assertIs(missing_oracle.status, Status.OOS)
        self.assertIn("not present in canvas2d oracle", missing_oracle.detail)

    def test_oracle_expected_missing_and_partial_short_circuit(self) -> None:
        missing = self.adapter.run(
            _entry("createPattern", status="missing", notes="shim returns null")
        )
        self.assertIs(missing.status, Status.NOT_IMPL)
        self.assertIn("oracle marks", missing.detail)

        partial = self.adapter.run(
            _entry("transform", status="partial", unsupported_values=["rotation"])
        )
        self.assertIs(partial.status, Status.DIVERGE)
        self.assertEqual(partial.extra_unsupported, ["rotation"])

    def test_maps_to_unimplemented_and_noop_paths(self) -> None:
        unimplemented = self.adapter.run(
            _entry("fillRect", status="missing", maps_to="not implemented in shim or bridge")
        )
        self.assertIs(unimplemented.status, Status.NOT_IMPL)
        self.assertIn("mapsTo signals no implementation", unimplemented.detail)

        noop = self.adapter.run(
            _entry("missingBridge", status="noop", maps_to="accepted but ignored")
        )
        self.assertIs(noop.status, Status.NO_OP)
        self.assertIn("bridge missing", noop.detail)

    def test_bridge_and_shim_presence_checks(self) -> None:
        missing_bridge = self.adapter.run(_entry("missingBridge", status="missing"))
        self.assertIs(missing_bridge.status, Status.NOT_IMPL)
        self.assertEqual(missing_bridge.unmatched_supported, ["canvasMissingBridge"])

        missing_shim = self.adapter.run(_entry("missingShim", status="missing"))
        self.assertIs(missing_shim.status, Status.NOT_IMPL)
        self.assertIn("shim does not declare", missing_shim.detail)

    def test_cited_bridge_mismatch_diverges(self) -> None:
        result = self.adapter.run(
            _entry("citesMissing", status="partial", maps_to="routes through canvasNotRegistered")
        )

        self.assertIs(result.status, Status.DIVERGE)
        self.assertEqual(result.extra_unsupported, ["canvasNotRegistered"])

    def test_attribute_values_pass_diverge_and_fallback_to_oracle_set(self) -> None:
        empty_supported_values = self.adapter.run(_entry("lineCap"))
        self.assertIs(empty_supported_values.status, Status.PASS)
        self.assertIn("oracle values", empty_supported_values.detail)

        all_values = self.adapter.run(
            _entry("lineCap", supported_values=["butt", "round", "square"])
        )
        self.assertIs(all_values.status, Status.PASS)
        self.assertEqual(all_values.matched_supported, ["butt", "round", "square"])

        unsupported_overlap = self.adapter.run(
            _entry(
                "textAlign",
                status="partial",
                supported_values=["left", "center"],
                unsupported_values=["right"],
            )
        )
        self.assertIs(unsupported_overlap.status, Status.DIVERGE)
        self.assertEqual(unsupported_overlap.extra_unsupported, ["right"])

        partial_supported_values = self.adapter.run(
            _entry("lineJoin", status="partial", supported_values=["miter"])
        )
        self.assertIs(partial_supported_values.status, Status.DIVERGE)
        self.assertEqual(partial_supported_values.unmatched_supported, ["bevel", "round"])

        unsupported_without_supported = self.adapter.run(
            _entry("lineCap", status="partial", unsupported_values=["round"])
        )
        self.assertIs(unsupported_without_supported.status, Status.DIVERGE)
        self.assertEqual(unsupported_without_supported.extra_unsupported, ["round"])

        non_overlapping_supported_values = self.adapter.run(
            _entry("lineJoin", supported_values=["invalid"])
        )
        self.assertIs(non_overlapping_supported_values.status, Status.PASS)

    def test_generic_method_and_attribute_classification(self) -> None:
        attr_without_initializer = self.adapter.run(_entry("globalAlpha"))
        self.assertIs(attr_without_initializer.status, Status.PASS)

        generic_unsupported = self.adapter.run(
            _entry("shadowBlur", status="partial", unsupported_values=["negative"])
        )
        self.assertIs(generic_unsupported.status, Status.DIVERGE)
        self.assertEqual(generic_unsupported.extra_unsupported, ["negative"])

        method_pass = self.adapter.run(_entry("fillRect"))
        self.assertIs(method_pass.status, Status.PASS)
        self.assertIn("bound via", method_pass.detail)


if __name__ == "__main__":
    unittest.main()
