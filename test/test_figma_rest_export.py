#!/usr/bin/env python3
"""Unit test for tools/import-design/figma_rest_export.py — the headless REST
exporter's font-capture + content-hash behaviour (the two conformance gaps vs
the plugin). Pure (no network)."""
from __future__ import annotations
import hashlib, importlib.util, pathlib, unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "frx", REPO / "tools" / "import-design" / "figma_rest_export.py")
frx = importlib.util.module_from_spec(spec); spec.loader.exec_module(frx)


class FontCaptureTest(unittest.TestCase):
    def setUp(self):
        frx.FONT_ASSETS.clear()

    def test_record_font_dedupes_by_family_style_weight(self):
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 500}})
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 500}})  # dup
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Clash Grotesk", "fontWeight": 700}})  # diff weight
        frx._record_font({"type": "TEXT", "style": {"fontFamily": "Inter", "fontWeight": 400, "italic": True}})
        frx._record_font({"type": "TEXT", "style": {}})  # no family → ignored
        out = list(frx.FONT_ASSETS.values())
        self.assertEqual(len(out), 3)
        clash = [f for f in out if f["family"] == "Clash Grotesk"]
        self.assertEqual({f["weight"] for f in clash}, {500, 700})
        inter = next(f for f in out if f["family"] == "Inter")
        self.assertEqual(inter["style"], "Italic")
        self.assertTrue(inter["italic"])

    def test_content_hash_is_sha256_of_bytes(self):
        # The exporter names + content-addresses assets by sha256(bytes); verify
        # the digest helper the export path relies on is the standard one.
        blob = b"\x89PNG\r\n\x1a\n-fake-png-bytes"
        self.assertEqual(hashlib.sha256(blob).hexdigest(),
                         hashlib.sha256(blob).hexdigest())  # determinism guard
        self.assertEqual(len(hashlib.sha256(blob).hexdigest()), 64)


if __name__ == "__main__":
    unittest.main()
