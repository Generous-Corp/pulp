#!/usr/bin/env python3
"""No-network tests for desktop action geometry helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_geometry.py")


class DesktopActionGeometryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_parse_coordinate_pair_accepts_numeric_x_y(self) -> None:
        self.assertEqual(self.mod.parse_coordinate_pair(" 10.5, 20 ", flag_name="--click"), (10.5, 20.0))

        with self.assertRaisesRegex(ValueError, "X,Y form"):
            self.mod.parse_coordinate_pair("10", flag_name="--click")
        with self.assertRaisesRegex(ValueError, "numeric"):
            self.mod.parse_coordinate_pair("x,y", flag_name="--click")

    def test_screen_point_and_content_size_helpers_preserve_existing_inset_policy(self) -> None:
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 100, "y": 50, "width": 400, "height": 300}},
                (200, 180),
                (10, 20),
            ),
            (210.0, 190.0),
        )
        self.assertEqual(self.mod.content_size_from_window({"bounds": {"width": "400", "height": 250}}), (400.0, 250.0))
        self.assertEqual(
            self.mod.content_size_from_view_tree({"bounds": {"width": 320, "height": 0}}, (100.0, 200.0)),
            (320.0, 200.0),
        )


if __name__ == "__main__":
    unittest.main()
