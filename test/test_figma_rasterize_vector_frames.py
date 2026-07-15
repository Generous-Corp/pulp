#!/usr/bin/env python3
"""Unit tests for the figma vector-frame rasterizer helper.

These are pure tests: no Figma token and no network access required.
"""
from __future__ import annotations

import hashlib
import importlib.util
import pathlib
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "frvf", REPO / "tools" / "import-design" / "figma_rasterize_vector_frames.py")
frvf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frvf)


class RasterizeVectorFramesTest(unittest.TestCase):
    def test_collect_targets_returns_topmost_vector_illustrations(self):
        root = {
            "type": "frame",
            "name": "Root",
            "children": [
                {
                    "type": "frame",
                    "name": "Triangle",
                    "figma_node_id": "3:236",
                    "children": [
                        {
                            "type": "group",
                            "name": "Nested vector face",
                            "figma_node_id": "3:237",
                            "children": [
                                {
                                    "type": "frame",
                                    "name": "Polygon face",
                                    "figma": {"figma_type": "REGULAR_POLYGON"},
                                }
                            ],
                        }
                    ],
                },
                {
                    "type": "frame",
                    "name": "Knob Row",
                    "figma_node_id": "9:1",
                    "children": [
                        {
                            "type": "frame",
                            "name": "Vector face",
                            "figma": {"figma_type": "REGULAR_POLYGON"},
                        }
                    ],
                },
                {
                    "type": "frame",
                    "name": "Labeled art",
                    "figma_node_id": "9:2",
                    "children": [{"type": "text", "name": "Value"}],
                },
            ],
        }

        targets = frvf.collect_targets(root)

        self.assertEqual([t["figma_node_id"] for t in targets], ["3:236"])
        self.assertEqual(targets[0]["name"], "Triangle")

    def test_apply_flattened_asset_updates_node_and_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)
            assets = base / "assets"
            assets.mkdir()
            png = assets / "3_236.png"
            blob = b"\x89PNG\r\n\x1a\nflattened-vector-frame"
            png.write_bytes(blob)
            out_scene = base / "scene.rasterized.pulp.json"

            scene = {
                "asset_manifest": {
                    "version": 1,
                    "assets": [
                        {
                            "asset_id": "3:236",
                            "original_uri": "figma://old/3:236",
                            "original_uri_aliases": ["figma://alias/3:236"],
                            "local_path": "old.png",
                            "content_hash": "old",
                            "mime": "image/png",
                        }
                    ],
                }
            }
            node = {
                "type": "frame",
                "figma_node_id": "3:236",
                "children": [{"type": "frame", "name": "degraded"}],
                "attributes": {"asset_path": "/tmp/stale.png", "role": "art"},
            }

            entry = frvf.apply_flattened_asset(scene, node,
                                               file_key="FILEKEY",
                                               asset_path=str(png),
                                               output_scene_path=str(out_scene))

        self.assertEqual(node["type"], "image")
        self.assertEqual(node["asset_ref"], "3:236")
        self.assertEqual(node["children"], [])
        self.assertNotIn("asset_path", node["attributes"])
        self.assertEqual(node["attributes"]["role"], "art")

        self.assertEqual(entry["asset_id"], "3:236")
        self.assertEqual(entry["original_uri"], "figma://FILEKEY/3:236")
        self.assertEqual(entry["original_uri_aliases"], ["figma://alias/3:236"])
        self.assertEqual(entry["local_path"], "assets/3_236.png")
        self.assertEqual(entry["content_hash"], hashlib.sha256(blob).hexdigest())
        self.assertEqual(entry["mime"], "image/png")
        self.assertEqual(len(scene["asset_manifest"]["assets"]), 1)

    def test_upsert_asset_manifest_creates_missing_manifest(self):
        scene = {}

        entry = frvf.upsert_asset_manifest_entry(scene,
                                                 asset_id="3:245",
                                                 local_path="assets/3_245.png",
                                                 file_key="FILEKEY",
                                                 content_hash="abc123")

        self.assertEqual(scene["asset_manifest"]["version"], 1)
        self.assertEqual(scene["asset_manifest"]["assets"], [entry])
        self.assertEqual(entry["asset_id"], "3:245")
        self.assertEqual(entry["original_uri"], "figma://FILEKEY/3:245")
        self.assertEqual(entry["original_uri_aliases"], [])
        self.assertEqual(entry["local_path"], "assets/3_245.png")
        self.assertEqual(entry["content_hash"], "abc123")
        self.assertEqual(entry["mime"], "image/png")


    def test_default_scale_matches_materializer_export_scale(self):
        # The native materializer hardcodes kExportScale = 2.0 (design_import.cpp).
        # A larger producer scale trips its asset_bleed heuristic on a frame that
        # is rasterized to its own bounds, so the default must stay aligned.
        self.assertEqual(frvf.build_parser().get_default("scale"), 2.0)


if __name__ == "__main__":
    unittest.main()
