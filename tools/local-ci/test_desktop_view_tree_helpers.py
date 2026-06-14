#!/usr/bin/env python3
"""No-network tests for desktop action view-tree helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_view_tree_helpers.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopViewTreeHelpersTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_view_tree_helpers_resolve_visible_node_centers(self) -> None:
        view_tree = {
            "id": "root",
            "type": "panel",
            "bounds": {"x": 10, "y": 20, "width": 200, "height": 100},
            "children": [
                {"id": "hidden", "visible": False, "bounds": {"x": 1, "y": 1, "width": 20, "height": 20}},
                {"id": "zero", "type": "button", "bounds": {"x": 2, "y": 2, "width": 0, "height": 20}},
                {
                    "id": "panel",
                    "bounds": {"x": 5, "y": 6, "width": 100, "height": 80},
                    "children": [
                        {
                            "id": "target",
                            "type": "button",
                            "text": "OK",
                            "label": "Confirm",
                            "bounds": {"x": 7, "y": 8, "width": 30, "height": 10},
                        },
                    ],
                },
            ],
        }

        nodes = list(self.mod.iter_view_tree_nodes(view_tree))
        self.assertEqual(list(self.mod.iter_view_tree_nodes("not-a-node")), [])
        self.assertEqual(nodes[-1][1], {"x": 22.0, "y": 34.0, "width": 30.0, "height": 10.0})
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="target",
                view_type="button",
                view_text="OK",
                view_label="Confirm",
            ),
            (37.0, 39.0),
        )
        self.assertEqual(self.mod.view_tree_inspector_summary(view_tree)["view_count"], 5)
        with self.assertRaisesRegex(RuntimeError, "No visible view matched"):
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="missing",
                view_type=None,
                view_text=None,
                view_label=None,
            )

    def test_count_view_tree_nodes_handles_non_tree_children(self) -> None:
        self.assertEqual(self.mod.count_view_tree_nodes("not-a-node"), 0)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": "bad"}), 1)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": [{"children": [{}]}, {}]}), 4)


if __name__ == "__main__":
    unittest.main()
