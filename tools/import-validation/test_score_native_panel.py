#!/usr/bin/env python3
"""Tests for score_native_panel.py's computed-style predicates.

These exist because of a specific failure, and the test is shaped to catch that
failure's whole family rather than the one instance:

`background-blend-mode` serializes ONE VALUE PER BACKGROUND LAYER, so a node
with two background layers and no blending computes to `"normal, normal"`. The
classifier compared the whole value against `"normal"`, so every multi-layer
background was labelled a blend. Because a soft screen-sized wash is written as
exactly that — two stacked radial gradients — this mislabelled the single
largest node on three of four captured designs, and produced a confident,
actionable-looking "59-70% of failing area is background-blend-mode" for a
corpus containing no non-normal blend mode at all.

The same wrong comparison independently corrupted a second instrument, so the
tests below cover the whole per-layer family and both directions: a real
non-initial layer must still register, or fixing the false positive would just
trade it for a false negative.

Run: python3 tools/import-validation/test_score_native_panel.py
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import score_native_panel as S  # noqa: E402


class TestSplitLayers(unittest.TestCase):
    def test_splits_on_top_level_commas(self):
        self.assertEqual(S.split_layers("normal, normal"), ["normal", "normal"])
        self.assertEqual(S.split_layers("none"), ["none"])

    def test_a_comma_inside_a_gradient_separates_stops_not_layers(self):
        # The whole reason the split has to be paren-aware: a naive split would
        # shred one gradient into four bogus "layers", and every one of them
        # would then fail an initial-value comparison.
        one = "linear-gradient(90deg, rgb(1, 2, 3) 0%, rgb(4, 5, 6) 100%)"
        self.assertEqual(S.split_layers(one), [one])

    def test_two_gradients_are_two_layers(self):
        two = ("radial-gradient(90% 70% at 50% 30%, rgba(1, 2, 3, 0.1), "
               "rgba(0, 0, 0, 0) 60%), "
               "radial-gradient(60% 50% at 80% 90%, rgba(4, 5, 6, 0.1), "
               "rgba(0, 0, 0, 0))")
        self.assertEqual(len(S.split_layers(two)), 2)


class TestAllLayersInitial(unittest.TestCase):
    def test_a_multi_layer_initial_value_is_still_initial(self):
        # The exact string that caused the misreport.
        self.assertTrue(S.all_layers_initial("normal, normal", "normal", ""))
        self.assertTrue(S.all_layers_initial("normal, normal, normal",
                                             "normal", ""))
        self.assertTrue(S.all_layers_initial("none, none", "none"))

    def test_a_real_non_initial_layer_still_registers(self):
        # The inverse error. Suppressing the false positive by looking only at
        # the first layer would silently swallow every one of these.
        self.assertFalse(S.all_layers_initial("normal, overlay", "normal", ""))
        self.assertFalse(S.all_layers_initial("overlay, normal", "normal", ""))
        self.assertFalse(S.all_layers_initial("overlay, overlay", "normal", ""))
        self.assertFalse(S.all_layers_initial("multiply", "normal", ""))


class TestBlends(unittest.TestCase):
    def test_two_plain_background_layers_do_not_blend(self):
        self.assertFalse(S.blends({"background-blend-mode": "normal, normal",
                                   "mix-blend-mode": "normal"}))

    def test_a_declared_background_blend_mode_blends(self):
        self.assertTrue(S.blends({"background-blend-mode": "normal, overlay",
                                  "mix-blend-mode": "normal"}))

    def test_mix_blend_mode_is_scalar_and_still_registers(self):
        # mix-blend-mode applies to the whole element, not per layer, so it is
        # compared as a scalar deliberately — the per-layer fix must not make
        # it unreachable.
        self.assertTrue(S.blends({"background-blend-mode": "normal, normal",
                                  "mix-blend-mode": "overlay"}))

    def test_absent_properties_do_not_blend(self):
        self.assertFalse(S.blends({}))


class TestFeatureClass(unittest.TestCase):
    @staticmethod
    def node(styles, tag="DIV", text="", reasons=()):
        return {"styles": styles, "tag": tag, "text": text,
                "ink_reasons": list(reasons)}

    def test_a_two_layer_gradient_wash_classifies_as_gradient_not_blend(self):
        # The forge / forge-modular node that owned the failing-area table.
        st = {
            "background-blend-mode": "normal, normal",
            "mix-blend-mode": "normal",
            "background-image": (
                "radial-gradient(90% 70% at 50% 30%, rgba(94, 120, 255, 0.1), "
                "rgba(0, 0, 0, 0) 60%), "
                "radial-gradient(60% 50% at 80% 90%, rgba(139, 108, 245, 0.1), "
                "rgba(0, 0, 0, 0))"),
        }
        self.assertEqual(S.feature_class(self.node(st)), "gradient")

    def test_a_genuine_overlay_still_classifies_as_blend(self):
        st = {"background-blend-mode": "normal", "mix-blend-mode": "overlay",
              "background-image": "url(\"data:image/svg+xml;utf8,<svg/>\")"}
        self.assertEqual(S.feature_class(self.node(st)), "blend")

    def test_an_unrounded_box_is_not_classified_as_radius(self):
        # border-radius is not a layer list but has the same shape of trap: an
        # unrounded box serializes all four corners, so "0px 0px 0px 0px" is
        # not equal to the scalar initial "0px".
        self.assertEqual(
            S.feature_class(self.node({"border-radius": "0px 0px 0px 0px"})),
            "fill")
        self.assertEqual(
            S.feature_class(self.node({"border-radius": "0px"})), "fill")

    def test_a_rounded_box_still_classifies_as_radius(self):
        self.assertEqual(
            S.feature_class(self.node({"border-radius": "0px 8px 0px 0px"})),
            "radius")
        self.assertEqual(
            S.feature_class(self.node({"border-radius": "50%"})), "radius")


class TestInkCoverage(unittest.TestCase):
    """The companion statistic, against its own controls.

    Area-weighted failing cannot tell a better render from a blanker one, so
    coverage exists to catch that. It needs its own controls for the same
    reason: a coverage number that reads well for a blank render would be
    worse than no number.
    """

    def setUp(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy required")
        self.np = np
        # A mostly-dark reference with a bright patch, i.e. the shape of panel
        # the metric is blind on.
        ref = np.zeros((40, 40, 3), dtype=np.uint8)
        ref[:, :] = (5, 7, 10)
        ref[10:20, 10:20] = (200, 200, 200)
        self.ref = ref

    def test_identity_is_full_coverage(self):
        c = S.ink_coverage(self.ref, self.ref)
        self.assertAlmostEqual(c["covered"], 1.0, places=6)
        self.assertAlmostEqual(c["ink_ratio"], 1.0, places=6)

    def test_a_blank_render_has_no_coverage(self):
        # This is the case the failing fraction scores WELL on: matching the
        # dark modal colour everywhere.
        blank = self.np.zeros_like(self.ref)
        blank[:, :] = (5, 7, 10)
        c = S.ink_coverage(self.ref, blank)
        self.assertEqual(c["covered"], 0.0)
        self.assertEqual(c["ink_ratio"], 0.0)

    def test_a_flood_is_caught_by_ink_ratio_not_by_coverage(self):
        # Why both numbers are reported. A solid-black render is nowhere near
        # the modal colour, so it "covers" every ink pixel — coverage alone
        # would certify it.
        flood = self.np.zeros_like(self.ref)
        c = S.ink_coverage(self.ref, flood)
        self.assertAlmostEqual(c["covered"], 1.0, places=6)
        self.assertGreater(c["ink_ratio"], 5.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
