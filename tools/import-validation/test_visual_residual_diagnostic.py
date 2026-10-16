import unittest

try:
    from PIL import Image, ImageDraw
    from visual_residual_diagnostic import measure
except ModuleNotFoundError:  # The tool's optional Pillow dependency is absent in some gates.
    Image = ImageDraw = measure = None


@unittest.skipUnless(Image is not None, "Pillow is unavailable")
class VisualResidualDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.reference = Image.new("RGBA", (24, 16), (20, 20, 20, 255))
        ImageDraw.Draw(self.reference).rectangle((6, 4, 17, 11), fill=(220, 220, 220, 255))

    def test_identical_images_have_no_residual(self):
        report = measure(self.reference, self.reference.copy(), root=(2, 2, 20, 12))
        self.assertEqual(report["schema"], "pulp-visual-residual-diagnostic-v1")
        self.assertEqual(sum(row["differing_pixels"] for row in report["categories"].values()), 0)

    def test_interior_colour_change_is_not_mask_disagreement(self):
        candidate = self.reference.copy()
        ImageDraw.Draw(candidate).rectangle((9, 6, 14, 9), fill=(180, 180, 180, 255))
        report = measure(self.reference, candidate, root=(2, 2, 20, 12), edge_width=1)
        interior = report["categories"]["shared_foreground_interior"]
        self.assertGreater(interior["differing_pixels"], 0)
        self.assertEqual(report["categories"]["mask_or_placement_interior"]["differing_pixels"], 0)

    def test_shifted_shape_is_seen_as_edge_or_mask_difference(self):
        candidate = Image.new("RGBA", self.reference.size, (20, 20, 20, 255))
        ImageDraw.Draw(candidate).rectangle((7, 4, 18, 11), fill=(220, 220, 220, 255))
        report = measure(self.reference, candidate, root=(2, 2, 20, 12), edge_width=1)
        edge = report["categories"]["edge_band_residual"]["differing_pixels"]
        mask = report["categories"]["mask_or_placement_interior"]["differing_pixels"]
        self.assertGreater(edge + mask, 0)

    def test_alpha_only_change_is_counted(self):
        candidate = self.reference.copy()
        candidate.putpixel((10, 7), (220, 220, 220, 100))
        report = measure(self.reference, candidate)
        self.assertEqual(report["categories"]["shared_foreground_interior"]["differing_pixels"], 1)
        self.assertEqual(report["categories"]["shared_foreground_interior"]["alpha_only_differing_pixels"], 1)

    def test_subthreshold_alpha_change_is_not_counted_as_difference(self):
        candidate = self.reference.copy()
        candidate.putpixel((10, 7), (220, 220, 220, 254))
        report = measure(self.reference, candidate, difference_threshold=8)
        self.assertEqual(report["categories"]["shared_foreground_interior"]["differing_pixels"], 0)
        self.assertEqual(report["categories"]["shared_foreground_interior"]["alpha_only_differing_pixels"], 0)

    def test_background_shift_is_reported_without_claiming_cause(self):
        candidate = Image.new("RGBA", self.reference.size, (24, 24, 24, 255))
        ImageDraw.Draw(candidate).rectangle((6, 4, 17, 11), fill=(220, 220, 220, 255))
        report = measure(self.reference, candidate)
        self.assertTrue(report["diagnostic_only"])
        self.assertEqual(report["classification_note"], "heuristic regions; not causal renderer attribution")
        self.assertGreater(report["categories"]["background_residual"]["differing_pixels"], 0)

    def test_removed_shape_is_mask_disagreement(self):
        candidate = Image.new("RGBA", self.reference.size, (20, 20, 20, 255))
        ImageDraw.Draw(candidate).rectangle((6, 4, 11, 11), fill=(220, 220, 220, 255))
        report = measure(self.reference, candidate)
        self.assertGreater(report["categories"]["mask_or_placement_interior"]["differing_pixels"], 0)

    def test_measurement_is_deterministic(self):
        candidate = self.reference.copy()
        ImageDraw.Draw(candidate).point((0, 0), fill=(255, 0, 0, 255))
        self.assertEqual(measure(self.reference, candidate), measure(self.reference, candidate))

    def test_outside_root_is_separated(self):
        candidate = self.reference.copy()
        ImageDraw.Draw(candidate).point((0, 0), fill=(255, 0, 0, 255))
        report = measure(self.reference, candidate, root=(2, 2, 20, 12))
        self.assertEqual(report["categories"]["outside_root_registration"]["differing_pixels"], 1)

    def test_rejects_mismatched_dimensions_and_root(self):
        with self.assertRaises(ValueError):
            measure(self.reference, Image.new("RGBA", (1, 1)))
        with self.assertRaises(ValueError):
            measure(self.reference, self.reference, root=(0, 0, 25, 16))


if __name__ == "__main__":
    unittest.main()
