#!/usr/bin/env python3
"""Tests for desktop action geometry helper bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_action_geometry_bindings.py")


class DesktopActionGeometryBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_geometry_exports_match_wrappers(self) -> None:
        expected = (
            "parse_coordinate_pair",
            "screen_point_for_content_point",
        )

        self.assertEqual(self.mod.DESKTOP_ACTION_GEOMETRY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_geometry_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        actions = types.SimpleNamespace(
            parse_coordinate_pair=capture("coord", (1.0, 2.0)),
            screen_point_for_content_point=capture("screen", (30.0, 40.0)),
        )
        bindings = {"_desktop_actions": actions}

        self.assertEqual(self.mod.parse_coordinate_pair(bindings, "1,2", flag_name="--click"), (1.0, 2.0))
        self.assertEqual(captured["coord"], (("1,2",), {"flag_name": "--click"}))
        self.assertEqual(
            self.mod.screen_point_for_content_point(bindings, {"bounds": {}}, (100, 50), (1, 2)),
            (30.0, 40.0),
        )
        self.assertEqual(captured["screen"][0], ({"bounds": {}}, (100, 50), (1, 2)))

if __name__ == "__main__":
    unittest.main()
