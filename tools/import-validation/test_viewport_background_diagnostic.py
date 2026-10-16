from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

from PIL import Image

SCRIPT = Path(__file__).with_name("viewport_background_diagnostic.py")
SPEC = importlib.util.spec_from_file_location("viewport_background_diagnostic", SCRIPT)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class ViewportBackgroundDiagnosticTest(unittest.TestCase):
    def test_records_corners_and_outside_root_without_scoring(self) -> None:
        reference = Image.new("RGBA", (4, 4), (10, 20, 30, 255))
        candidate = Image.new("RGBA", (4, 4), (10, 20, 30, 255))
        candidate.putpixel((0, 0), (99, 88, 77, 255))
        candidate.putpixel((3, 3), (99, 88, 77, 255))
        result = module.measure(reference, candidate, (1, 1, 2, 2))
        self.assertEqual(result["schema"], "pulp-viewport-background-diagnostic-v1")
        self.assertEqual(result["corners"]["top_left"]["candidate"], [99, 88, 77])
        outside = result["outside_root"]
        self.assertEqual(outside["pixel_count"], 12)
        self.assertEqual(outside["differing_pixels"], 2)

    def test_rejects_root_outside_capture(self) -> None:
        image = Image.new("RGBA", (4, 4), "black")
        with self.assertRaises(ValueError):
            module.measure(image, image, (0, 0, 5, 4))


if __name__ == "__main__":
    unittest.main()
