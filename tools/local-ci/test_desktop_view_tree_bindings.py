#!/usr/bin/env python3
"""Tests for desktop view-tree helper bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_view_tree_bindings.py")


class DesktopViewTreeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_view_tree_exports_match_wrappers(self) -> None:
        expected = (
            "count_view_tree_nodes",
            "iter_view_tree_nodes",
            "resolve_view_tree_click_point",
        )

        self.assertEqual(self.mod.DESKTOP_VIEW_TREE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_view_tree_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        actions = types.SimpleNamespace(
            count_view_tree_nodes=capture("count", 3),
            iter_view_tree_nodes=lambda *args, **kwargs: iter(captured.setdefault("iter", (args, kwargs)) and [("node", {})]),
            resolve_view_tree_click_point=capture("resolve", (10.0, 20.0)),
        )
        bindings = {"_desktop_actions": actions}
        node = {"children": []}

        self.assertEqual(self.mod.count_view_tree_nodes(bindings, node), 3)
        self.assertEqual(captured["count"][0], (node,))
        self.assertEqual(list(self.mod.iter_view_tree_nodes(bindings, node, offset_x=1.0, offset_y=2.0)), [("node", {})])
        self.assertEqual(captured["iter"], ((node,), {"offset_x": 1.0, "offset_y": 2.0}))
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

if __name__ == "__main__":
    unittest.main()
