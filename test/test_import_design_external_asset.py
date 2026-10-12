#!/usr/bin/env python3
"""pulp-import-design copies an external local asset into the saved document.

A design may name an asset by a path outside its own directory
(`../external/art.png`). The saved DesignIR is loaded later with the
document's directory as its only asset root, and the loader refuses any path
that leaves it. So the importer must carry such a file into the output's
`assets/` under its content hash and record that relative path, or the saved
document loses the image.

The cases, end to end through the CLI and the observer:
  - the external file lands in `<output>/assets/<sha256>.png`, the manifest
    names it relatively, and its bytes hash to the recorded content_hash;
  - with the external source deleted, the saved document still renders the
    image, so the render reads nothing outside the document;
  - a missing external file still reports `asset-unresolved` and records no
    path.

Usage: test_import_design_external_asset.py --importer <pulp-import-design>
                                            --observer <pulp-design-ir-observe>
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

IMAGE = Path(__file__).resolve().parent / "fixtures/import-fidelity/assets/knob_ref.png"
IMPORTER: Path | None = None
OBSERVER: Path | None = None

DESIGN = {
    "type": "frame", "name": "Screen",
    "style": {"width": 120, "height": 120, "backgroundColor": "#202428"},
    "children": [{"type": "image", "name": "Art", "style": {"width": 60, "height": 100},
                  "attributes": {"src": "../external/art.png"}}],
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ExternalAssetTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "src").mkdir()
        (self.root / "external").mkdir()
        (self.root / "src" / "design.json").write_text(json.dumps(DESIGN), encoding="utf-8")
        self.output = self.root / "out" / "design.ir.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def import_design(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(IMPORTER), "--from", "stitch", "--file", str(self.root / "src" / "design.json"),
             "--emit", "ir-json", "--output", str(self.output)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)

    def render(self, name: str) -> tuple[bytes, str]:
        png = self.root / f"{name}.png"
        result = subprocess.run(
            [str(OBSERVER), "--input", str(self.output), "--render", str(png),
             "--layout", str(self.root / f"{name}.json"), "--width", "120", "--height", "120"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr[-400:])
        return png.read_bytes(), result.stderr

    def manifest_asset(self) -> dict:
        assets = json.loads(self.output.read_text(encoding="utf-8"))["assetManifest"]["assets"]
        self.assertEqual(len(assets), 1, assets)
        return assets[0]

    def test_external_asset_is_copied_in_and_renders_without_its_source(self) -> None:
        source = self.root / "external" / "art.png"
        shutil.copy2(IMAGE, source)
        expected_hash = sha256(source)

        result = self.import_design()
        self.assertEqual(result.returncode, 0, result.stderr[-400:])

        asset = self.manifest_asset()
        self.assertEqual(asset["content_hash"], expected_hash)
        self.assertEqual(asset["local_path"], f"assets/{expected_hash}.png",
                         "the external file must be recorded by a document-relative path")
        copied = self.output.parent / asset["local_path"]
        self.assertTrue(copied.is_file(), "the external file must be copied into the document")
        self.assertEqual(sha256(copied), expected_hash, "the copy must keep the recorded hash")
        self.assertNotIn(str(self.root / "external"), self.output.read_text(encoding="utf-8"),
                         "no manifest path may point at the external source")

        # Delete the source, so any read outside the document finds nothing.
        shutil.rmtree(self.root / "external")
        painted, stderr = self.render("painted")
        self.assertNotIn("refused", stderr)
        shutil.rmtree(self.output.parent / "assets")
        absent, _ = self.render("absent")
        self.assertNotEqual(painted, absent,
                            "the saved document must paint the copied asset after reload")

    def test_missing_external_file_still_reports_unresolved(self) -> None:
        result = self.import_design()
        self.assertEqual(result.returncode, 0, result.stderr[-400:])
        self.assertIn("asset-unresolved", result.stderr)
        asset = self.manifest_asset()
        self.assertNotIn("local_path", asset)
        self.assertFalse((self.output.parent / "assets").exists())


def main() -> int:
    global IMPORTER, OBSERVER
    parser = argparse.ArgumentParser()
    parser.add_argument("--importer", required=True, type=Path)
    parser.add_argument("--observer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    IMPORTER, OBSERVER = args.importer, args.observer
    return 0 if unittest.main(argv=[sys.argv[0], *rest], exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
