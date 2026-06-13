#!/usr/bin/env python3
"""Unit tests for gallery_visual_diff.changed_fraction — pure Python, no PIL,
so it runs on every CI lane (the end-to-end render+diff is a separate ctest that
needs Pillow + a render backend)."""
import unittest

from gallery_visual_diff import changed_fraction


class ChangedFraction(unittest.TestCase):
    def test_identical_is_zero(self):
        px = [(10, 20, 30, 255)] * 100
        self.assertEqual(changed_fraction(px, px), 0.0)

    def test_empty_is_zero(self):
        self.assertEqual(changed_fraction([], []), 0.0)

    def test_within_tolerance_not_counted(self):
        a = [(100, 100, 100, 255)] * 10
        b = [(105, 96, 100, 255)] * 10           # max delta 5 < tol 8
        self.assertEqual(changed_fraction(a, b, channel_tol=8), 0.0)

    def test_beyond_tolerance_counted(self):
        a = [(0, 0, 0, 255)] * 4
        b = [(0, 0, 0, 255), (0, 0, 0, 255), (50, 0, 0, 255), (0, 0, 0, 255)]
        self.assertEqual(changed_fraction(a, b, channel_tol=8), 0.25)

    def test_alpha_channel_considered(self):
        a = [(0, 0, 0, 255)] * 2
        b = [(0, 0, 0, 255), (0, 0, 0, 100)]     # alpha delta 155 > tol
        self.assertEqual(changed_fraction(a, b, channel_tol=8), 0.5)

    def test_size_mismatch_raises(self):
        with self.assertRaises(ValueError):
            changed_fraction([(0, 0, 0, 0)], [(0, 0, 0, 0)] * 2)


if __name__ == "__main__":
    unittest.main()
