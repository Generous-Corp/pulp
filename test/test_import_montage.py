#!/usr/bin/env python3
"""Smoke test for tools/import-design/montage.py — the labeled comparison
montage helper. Skips cleanly on a PIL-less interpreter (matches the
fidelity-diff harness test). Verifies labels are ON by default and that the
title bars add the expected vertical space."""
from __future__ import annotations
import importlib.util, pathlib, sys, tempfile, unittest

try:
    from PIL import Image  # noqa: F401
except Exception:  # pragma: no cover
    sys.stderr.write("Pillow not installed — skipping montage test\n")
    sys.exit(77)  # CTest SKIP_RETURN_CODE

REPO = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "montage", REPO / "tools" / "import-design" / "montage.py")
montage = importlib.util.module_from_spec(spec); spec.loader.exec_module(montage)


def _img(tmp, name, w, h, color):
    from PIL import Image
    p = pathlib.Path(tmp) / name
    Image.new("RGB", (w, h), color).save(p)
    return str(p)


class MontageTest(unittest.TestCase):
    def test_labeled_by_default_adds_title_space(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            a = _img(tmp, "a.png", 200, 100, (10, 20, 30))
            b = _img(tmp, "b.png", 200, 100, (30, 20, 10))
            out = str(pathlib.Path(tmp) / "m.png")
            montage.build_montage([("Ref", a), ("Render", b)], out,
                                  title_height=36, pad=14, panel_width=200)
            self.assertTrue(pathlib.Path(out).exists())
            h_labeled = Image.open(out).size[1]
            out2 = str(pathlib.Path(tmp) / "m2.png")
            montage.build_montage([("Ref", a), ("Render", b)], out2,
                                  labels=False, title_height=36, pad=14, panel_width=200)
            h_plain = Image.open(out2).size[1]
            # Two title bars => labeled montage is ~2*title_height taller.
            self.assertGreater(h_labeled, h_plain)
            self.assertGreaterEqual(h_labeled - h_plain, 2 * 36 - 2)

    def test_columns_side_by_side(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            a = _img(tmp, "a.png", 200, 100, (10, 20, 30))
            b = _img(tmp, "b.png", 200, 100, (30, 20, 10))
            out = str(pathlib.Path(tmp) / "m.png")
            montage.build_montage([("A", a), ("B", b)], out, columns=2, panel_width=200)
            w = Image.open(out).size[0]
            self.assertGreater(w, 200 * 2)  # two columns side by side




class LabelColonTest(unittest.TestCase):
    def test_label_with_colon_survives(self):
        # #3237: a label containing a colon must not be truncated.
        import tempfile, os
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "ref.png"); Image.new("RGB", (10, 10)).save(p)
            label, path = montage._parse_panel(f"{p}:1. Figma: source")
            self.assertEqual(label, "1. Figma: source")
            self.assertEqual(path, p)
            # bare path (no label) → stem label
            self.assertEqual(montage._parse_panel(p), ("ref", p))


if __name__ == "__main__":
    unittest.main()
