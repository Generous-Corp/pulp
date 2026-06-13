#!/usr/bin/env python3
"""No-network tests for local-ci desktop action helpers."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_actions.py")


def load_module():
    spec = importlib.util.spec_from_file_location("desktop_actions_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopActionsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_desktop_action_artifact_paths_use_stable_bundle_layout(self) -> None:
        bundle_dir = pathlib.Path("/tmp/local-ci-bundle")
        paths = self.mod.desktop_action_artifact_paths(bundle_dir)

        self.assertEqual(paths["screenshot"], bundle_dir / "screenshots" / "window.png")
        self.assertEqual(paths["before_screenshot"], bundle_dir / "screenshots" / "before.png")
        self.assertEqual(paths["diff_screenshot"], bundle_dir / "screenshots" / "diff.png")
        self.assertEqual(paths["ui_snapshot"], bundle_dir / "ui-tree.json")
        self.assertEqual(paths["video"], bundle_dir / "video" / "proof.mp4")
        self.assertEqual(paths["video_composed"], bundle_dir / "video" / "proof-composed.mp4")
        self.assertEqual(paths["video_issue"], bundle_dir / "video" / "proof.issue.mp4")
        self.assertEqual(paths["video_small"], bundle_dir / "video" / "proof.small.mp4")
        self.assertEqual(paths["video_metadata"], bundle_dir / "video" / "metadata.json")
        self.assertEqual(paths["video_composed_metadata"], bundle_dir / "video" / "composed-metadata.json")
        self.assertEqual(paths["video_issue_metadata"], bundle_dir / "video" / "issue-metadata.json")
        self.assertEqual(paths["video_small_metadata"], bundle_dir / "video" / "small-metadata.json")
        self.assertEqual(paths["video_poster"], bundle_dir / "video" / "poster.png")
        self.assertEqual(paths["stdout"], bundle_dir / "stdout.log")
        self.assertEqual(paths["stderr"], bundle_dir / "stderr.log")

    def test_desktop_action_artifact_paths_expand_explicit_output_path(self) -> None:
        paths = self.mod.desktop_action_artifact_paths(pathlib.Path("/tmp/local-ci-bundle"), "~/Desktop/window.png")

        self.assertEqual(paths["screenshot"], pathlib.Path.home() / "Desktop" / "window.png")
        self.assertEqual(paths["before_screenshot"], pathlib.Path("/tmp/local-ci-bundle") / "screenshots" / "before.png")

    def test_interaction_helpers_preserve_click_selector_shape(self) -> None:
        self.assertFalse(
            self.mod.desktop_interaction_requested(
                click_point=None,
                click_view_id=None,
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
            )
        )
        self.assertTrue(
            self.mod.desktop_interaction_requested(
                click_point=None,
                click_view_id="bypass",
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
            )
        )
        self.assertEqual(
            self.mod.desktop_click_selector(
                click_point="1,2",
                click_view_id="bypass",
                click_view_type="button",
                click_view_text="Bypass",
                click_view_label="Bypass toggle",
            ),
            {
                "id": "bypass",
                "type": "button",
                "text": "Bypass",
                "label": "Bypass toggle",
                "point": "1,2",
            },
        )
        self.assertEqual(
            self.mod.desktop_click_selector(click_view_id="bypass", include_point=False),
            {"id": "bypass", "type": None, "text": None, "label": None},
        )
        self.assertEqual(
            self.mod.pulp_app_interaction_summary(
                click_point="1,2",
                click_view_id="bypass",
                click_view_type="button",
                click_view_text="Bypass",
                click_view_label="Bypass toggle",
            ),
            {
                "mode": "pulp-app",
                "click": {
                    "selector": {
                        "id": "bypass",
                        "type": "button",
                        "text": "Bypass",
                        "label": "Bypass toggle",
                        "point": "1,2",
                    }
                },
            },
        )

    def test_parse_coordinate_pair_accepts_numeric_x_y(self) -> None:
        self.assertEqual(self.mod.parse_coordinate_pair(" 10.5, 20 ", flag_name="--click"), (10.5, 20.0))

        with self.assertRaisesRegex(ValueError, "X,Y form"):
            self.mod.parse_coordinate_pair("10", flag_name="--click")
        with self.assertRaisesRegex(ValueError, "numeric"):
            self.mod.parse_coordinate_pair("x,y", flag_name="--click")

    def test_view_tree_helpers_resolve_visible_node_centers(self) -> None:
        view_tree = {
            "id": "root",
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
        self.assertEqual(nodes[0][1]["x"], 10.0)
        self.assertEqual(nodes[0][1]["height"], 100.0)
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
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id=None,
                view_type="button",
                view_text="OK",
                view_label=None,
            ),
            (37.0, 39.0),
        )
        with self.assertRaisesRegex(RuntimeError, "No visible view matched"):
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="missing",
                view_type=None,
                view_text=None,
                view_label=None,
            )

    def test_screen_point_for_content_point_preserves_existing_inset_policy(self) -> None:
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 100, "y": 50, "width": 400, "height": 300}},
                (200, 180),
                (10, 20),
            ),
            (210.0, 190.0),
        )
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 5, "y": 6, "width": 20, "height": 10}},
                (40, 30),
                (3, 4),
            ),
            (8.0, 10.0),
        )

    def test_count_view_tree_nodes_handles_non_tree_children(self) -> None:
        self.assertEqual(self.mod.count_view_tree_nodes("not-a-node"), 0)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": "bad"}), 1)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": [{"children": [{}]}, {}]}), 4)

    def test_inspector_summary_and_content_size_helpers_preserve_fallbacks(self) -> None:
        view_tree = {
            "id": "root",
            "type": "panel",
            "bounds": {"width": 320, "height": 0},
            "children": [{"id": "child"}],
        }

        self.assertEqual(
            self.mod.view_tree_inspector_summary(view_tree),
            {"root_id": "root", "root_type": "panel", "view_count": 2},
        )
        self.assertEqual(
            self.mod.content_size_from_window({"bounds": {"width": "400", "height": 250}}),
            (400.0, 250.0),
        )
        self.assertEqual(self.mod.content_size_from_window({"bounds": {}}), (0.0, 0.0))
        self.assertEqual(self.mod.content_size_from_view_tree(view_tree, (100.0, 200.0)), (320.0, 200.0))
        self.assertEqual(self.mod.content_size_from_view_tree({"bounds": "bad"}, (100.0, 200.0)), (100.0, 200.0))

    def test_default_desktop_label_matches_bundle_or_command(self) -> None:
        self.assertEqual(self.mod.default_desktop_label(None), "desktop-run")
        self.assertEqual(self.mod.default_desktop_label(""), "desktop-run")
        self.assertEqual(self.mod.default_desktop_label("/Applications/TextEdit.app/Contents/MacOS/TextEdit"), "TextEdit")
        self.assertEqual(self.mod.default_desktop_label(None, bundle_id="com.apple.TextEdit"), "TextEdit")
        self.assertEqual(self.mod.default_desktop_label(None, bundle_id="com.example."), "com.example.")


if __name__ == "__main__":
    unittest.main()
