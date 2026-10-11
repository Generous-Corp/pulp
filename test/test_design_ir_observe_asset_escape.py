#!/usr/bin/env python3
"""pulp-design-ir-observe does not paint an asset from outside the document.

A DesignIR manifest names its assets relative to the document's directory. A
local_path that climbs out (`../escape.png`) used to be resolved and painted
when the file existed, so a crafted document could draw any image the host
could read. It must now be refused with a diagnostic and render as if absent,
while the same bytes inside the document still render.

Usage: test_design_ir_observe_asset_escape.py --observer <pulp-design-ir-observe>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

IMAGE = Path(__file__).resolve().parent / "fixtures/import-fidelity/assets/knob_ref.png"
OBSERVER: Path | None = None


def document(local_path: str) -> dict:
    return {
        "version": 1, "source": "figma-plugin",
        "assetManifest": {"version": 1, "assets": [{
            "asset_id": "art", "original_uri": "", "local_path": local_path, "content_hash": "",
            "mime": "image/png", "width": 60, "height": 100, "diagnostics": []}]},
        "root": {"type": "frame", "name": "root", "stable_anchor_id": "root",
                 "style": {"backgroundColor": "#202428", "width": 120, "height": 120},
                 "children": [{"type": "image", "name": "Art", "stable_anchor_id": "art",
                               "style": {"width": 60, "height": 100},
                               "attributes": {"asset_ref": "art"}, "children": []}]},
    }


class AssetEscapeTests(unittest.TestCase):
    def render(self, local_path: str) -> tuple[bytes, str]:
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            doc = root / "doc"
            (doc / "assets").mkdir(parents=True)
            shutil.copy2(IMAGE, root / "escape.png")
            shutil.copy2(IMAGE, doc / "assets" / "inside.png")
            (doc / "design.ir.json").write_text(json.dumps(document(local_path)), encoding="utf-8")
            result = subprocess.run(
                [str(OBSERVER), "--input", str(doc / "design.ir.json"), "--render", str(root / "r.png"),
                 "--layout", str(root / "l.json"), "--width", "120", "--height", "120"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr[-400:])
            return (root / "r.png").read_bytes(), result.stderr

    def test_escaping_path_is_refused_and_not_painted(self) -> None:
        inside, _ = self.render("assets/inside.png")
        missing, _ = self.render("assets/missing.png")
        escaped, stderr = self.render("../escape.png")
        self.assertNotEqual(inside, missing, "control: the image must be visible when it resolves")
        self.assertIn("refused", stderr)
        self.assertIn("outside the document directory", stderr)
        self.assertEqual(escaped, missing, "an escaping asset must render as if absent")


def main() -> int:
    global OBSERVER
    parser = argparse.ArgumentParser()
    parser.add_argument("--observer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    OBSERVER = args.observer
    return 0 if unittest.main(argv=[sys.argv[0], *rest], exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
