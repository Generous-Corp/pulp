#!/usr/bin/env python3
"""Tests for desktop action support dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_action_support_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionSupportBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_support_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_TARGET_SELECTION_EXPORTS,
            *self.mod.DESKTOP_VIEW_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ACTION_SUPPORT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_resolve_desktop_target_preserves_selection_errors(self) -> None:
        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local"},
                    "linux": {"adapter": "linux-xvfb", "enabled": False},
                }
            }
        }

        self.assertEqual(
            self.mod.resolve_desktop_target({}, config, "mac"),
            {"adapter": "macos-local"},
        )
        with self.assertRaisesRegex(ValueError, "Unknown desktop target 'windows'"):
            self.mod.resolve_desktop_target({}, config, "windows")
        with self.assertRaisesRegex(ValueError, "Desktop target 'linux' is disabled"):
            self.mod.resolve_desktop_target({}, config, "linux")

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

    def test_install_support_helpers_routes_each_group(self) -> None:
        actions = types.SimpleNamespace(
            count_view_tree_nodes=lambda node: 3,
            parse_coordinate_pair=lambda value, *, flag_name: (1.0, 2.0),
        )
        bindings = {"_desktop_actions": actions}
        config = {"desktop_automation": {"targets": {"mac": {"adapter": "macos-local"}}}}

        self.mod.install_desktop_action_support_helpers(
            bindings,
            ("resolve_desktop_target", "count_view_tree_nodes", "parse_coordinate_pair"),
        )

        self.assertEqual(bindings["resolve_desktop_target"](config, "mac"), {"adapter": "macos-local"})
        self.assertEqual(bindings["count_view_tree_nodes"]({"children": []}), 3)
        self.assertEqual(bindings["parse_coordinate_pair"]("1,2", flag_name="--click"), (1.0, 2.0))


if __name__ == "__main__":
    unittest.main()
