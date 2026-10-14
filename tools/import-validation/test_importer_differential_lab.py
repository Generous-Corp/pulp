"""Regression tests for importer differential measurement integrity."""

from pathlib import Path
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from importer_differential.core import visual_metrics


class VisualMetricsTests(unittest.TestCase):
    def test_alpha_only_difference_is_not_pixel_identical(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reference = root / "reference.png"
            candidate = root / "candidate.png"
            Image.new("RGBA", (1, 1), (10, 20, 30, 255)).save(reference)
            Image.new("RGBA", (1, 1), (10, 20, 30, 0)).save(candidate)

            report = visual_metrics(reference, candidate, root / "overlays")

            self.assertEqual(report["differing_pixel_count"], 1)
            self.assertEqual(report["mean_alpha_error"], 255.0)
            self.assertLess(report["score"], 1.0)


if __name__ == "__main__":
    unittest.main()
