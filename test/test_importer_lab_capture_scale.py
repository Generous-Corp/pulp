#!/usr/bin/env python3
"""The importer lab, end to end with a real browser, stays in CSS pixels.

The capture browser runs at the capture's device scale, and Chromium then
reports DOMSnapshot geometry in device pixels. Every lab lens and the native
lowering read that geometry as CSS pixels, so a capture that kept device
units lowered the panel at twice its size: the native render filled twice the
frame and the registered visual score of a holdout panel fell to 0.45. A
capture-against-screenshot comparison cannot see this, because the reference
PNG itself is right; only the native side is wrong.

One lab comparison of a small fixture checks the three things that broke:
  - the browser snapshot's root box is the captured logical size;
  - the native render has the reference's pixel dimensions;
  - registered geometry and the registered visual score agree with Chromium.

Skips (exit 77) when no compatible browser or no Pillow is available.

Usage: test_importer_lab_capture_scale.py --importer <pulp-import-design>
                                          --observer <pulp-design-ir-observe>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LAB = ROOT / "tools/import-validation/importer_differential_lab.py"
FIXTURE = ROOT / "test/fixtures/import-differential/01-basic-card.html"
IMPORTER: Path | None = None
OBSERVER: Path | None = None

# The fixture scores about 0.98 against Chromium; a panel drawn at twice its
# size lands far below this.
MIN_REGISTERED_VISUAL = 0.9
MIN_GEOMETRY = 0.98


def skip(reason: str) -> None:
    print(f"SKIP: {reason}")
    sys.exit(77)


class LabCaptureScaleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            import PIL  # noqa: F401
        except ImportError:
            skip("Pillow is not installed")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.tmp.name) / "basic-card"
        run = subprocess.run(
            [sys.executable, str(LAB), "compare", "--importer", str(IMPORTER),
             "--observer", str(OBSERVER), "--file", str(FIXTURE),
             "--viewport", "1280x800", "--timeout-seconds", "240",
             "--output", str(cls.out)],
            capture_output=True, text=True, encoding="utf-8", timeout=600)
        if run.returncode != 0:
            message = run.stdout + run.stderr
            if "browser" in message.lower() and (
                    "discover" in message.lower() or "unavailable" in message.lower()
                    or "no compatible" in message.lower()):
                skip("no compatible browser for the capture")
            raise AssertionError(f"lab compare failed ({run.returncode}):\n{message[-4000:]}")
        cls.report = json.loads((cls.out / "comparison/report.json").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_snapshot_root_is_the_logical_capture_size(self) -> None:
        capture = json.loads((self.out / "browser/capture.json").read_text(encoding="utf-8"))
        snapshot = json.loads((self.out / "browser/dom-snapshot.json").read_text(encoding="utf-8"))
        root_box = snapshot["documents"][0]["layout"]["bounds"][0]
        self.assertEqual(root_box[2], capture["reference"]["logical_width"])
        self.assertEqual(root_box[3], capture["reference"]["logical_height"])

    def test_native_render_has_the_reference_dimensions(self) -> None:
        from PIL import Image
        with Image.open(self.out / "browser/browser.png") as reference, \
                Image.open(self.out / "candidate/render.png") as native:
            self.assertEqual(native.size, reference.size)

    def test_registered_scores_agree_with_chromium(self) -> None:
        comparison = self.report["comparison"]
        self.assertGreaterEqual(comparison["geometry"]["score"], MIN_GEOMETRY)
        visual = comparison["visual"].get("registered_visual") or comparison["visual"]
        self.assertGreaterEqual(visual["score"], MIN_REGISTERED_VISUAL)


def main() -> None:
    global IMPORTER, OBSERVER
    parser = argparse.ArgumentParser()
    parser.add_argument("--importer", type=Path, required=True)
    parser.add_argument("--observer", type=Path, required=True)
    args, rest = parser.parse_known_args()
    IMPORTER, OBSERVER = args.importer, args.observer
    unittest.main(argv=[sys.argv[0], *rest])


if __name__ == "__main__":
    main()
