#!/usr/bin/env python3
"""Tests for desktop view/action facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_view_action_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopViewActionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_action_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        actions = types.SimpleNamespace(
            count_view_tree_nodes=capture("count", 3),
            parse_coordinate_pair=capture("coord", (1.0, 2.0)),
            iter_view_tree_nodes=lambda *args, **kwargs: iter(captured.setdefault("iter", (args, kwargs)) and [("node", {"x": 1})]),
            resolve_view_tree_click_point=capture("resolve", (10.0, 20.0)),
            screen_point_for_content_point=capture("screen", (30.0, 40.0)),
            default_desktop_label=capture("label", "Demo"),
        )
        bindings = {"_desktop_actions": actions}
        node = {"children": []}

        self.assertEqual(self.mod.count_view_tree_nodes(bindings, node), 3)
        self.assertEqual(self.mod.parse_coordinate_pair(bindings, "1,2", flag_name="--click"), (1.0, 2.0))
        self.assertEqual(list(self.mod.iter_view_tree_nodes(bindings, node, offset_x=4.0, offset_y=5.0)), [("node", {"x": 1})])
        iter_args, iter_kwargs = captured["iter"]
        self.assertEqual(iter_args, (node,))
        self.assertEqual(iter_kwargs, {"offset_x": 4.0, "offset_y": 5.0})
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                bindings,
                node,
                view_id="gain",
                view_type="Slider",
                view_text="Gain",
                view_label="Gain slider",
            ),
            (10.0, 20.0),
        )
        self.assertEqual(captured["resolve"][1]["view_label"], "Gain slider")
        self.assertEqual(self.mod.screen_point_for_content_point(bindings, {"bounds": {}}, (100, 50), (1, 2)), (30.0, 40.0))
        self.assertEqual(self.mod.default_desktop_label(bindings, "./Demo", bundle_id="com.example.Demo"), "Demo")
        self.assertEqual(captured["label"][0], ("./Demo",))
        self.assertEqual(captured["label"][1], {"bundle_id": "com.example.Demo"})


if __name__ == "__main__":
    unittest.main()
