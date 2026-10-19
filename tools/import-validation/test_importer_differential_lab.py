import tempfile
import unittest
from pathlib import Path


class VisualRegistrationTests(unittest.TestCase):
    def test_visual_registration_compares_authenticated_panel_frame(self) -> None:
        from importer_differential.core import visual_metrics
        from PIL import Image

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "browser.png"
            candidate = root / "native.png"
            browser = Image.new("RGBA", (12, 8), (5, 7, 11, 255))
            browser.paste(Image.new("RGBA", (8, 6), (255, 255, 255, 255)), (4, 2))
            browser.save(reference)
            native = Image.new("RGBA", (12, 8), (30, 30, 46, 255))
            native.paste(Image.new("RGBA", (8, 6), (255, 255, 255, 255)), (0, 0))
            native.save(candidate)
            result = visual_metrics(
                reference, candidate, root / "overlays", (12, 8),
                {"x": 2, "y": 1, "width": 4, "height": 3}, 2.0)
            self.assertEqual(result["registration"]["source"], "authenticated-authored-frame")
            self.assertLess(result["score"], 1.0)
            self.assertEqual(result["registered_visual"]["score"], 1.0)
            self.assertIn("browser", result["outside_frame_residual"])
            self.assertIn("native", result["outside_frame_residual"])

            shifted = Image.new("RGBA", (12, 8), (30, 30, 46, 255))
            shifted.paste(Image.new("RGBA", (8, 6), (255, 255, 255, 255)), (1, 0))
            shifted_path = root / "shifted.png"
            shifted.save(shifted_path)
            shifted_result = visual_metrics(
                reference, shifted_path, root / "shifted-overlays", (12, 8),
                {"x": 2, "y": 1, "width": 4, "height": 3}, 2.0)
            self.assertLess(shifted_result["registered_visual"]["score"], 0.995)

    def test_native_authored_frame_requires_panel_local_layout(self) -> None:
        from importer_differential.core import LabError, verify_native_authored_frame

        ir = {"root": {"stable_anchor_id": "browser:root",
                        "style": {"width": 4, "height": 3}}}
        layout = {"nodes": [{"id": "browser:root", "rect": {
            "x": 0, "y": 0, "w": 12, "h": 8}}]}
        frame = {"x": 0, "y": 0, "width": 4, "height": 3}
        self.assertEqual(
            verify_native_authored_frame(ir, layout, frame)["anchor"],
            "browser:root")
        shifted = {"nodes": [{"id": "browser:root", "rect": {
            "x": 1, "y": 0, "w": 12, "h": 8}}]}
        with self.assertRaises(LabError):
            verify_native_authored_frame(ir, shifted, frame)

    def test_promotion_uses_registered_visual_when_available(self) -> None:
        from importer_differential.core import promotion_recommendation

        structural = {"score": 1.0}
        geometry = {"score": 1.0}
        typography = {"score": 1.0}
        full = {"score": 1.0, "registered_visual": {"score": 0.9}}
        result = promotion_recommendation([], structural, geometry, typography, full)
        self.assertFalse(result["threshold_eligible"])
        self.assertEqual(result["visual_gate_score"], 0.9)
