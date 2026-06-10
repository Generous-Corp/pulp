#!/usr/bin/env python3
"""No-network tests for local-ci desktop CLI line helpers."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_cli.py")


def load_module():
    spec = importlib.util.spec_from_file_location("desktop_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_smoke_success_lines_include_artifacts_interaction_and_image_change(self) -> None:
        manifest = {
            "label": "demo",
            "pid": 123,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "diff_screenshot": "/tmp/diff.png",
                "image_change": {
                    "changed": True,
                    "method": "pillow",
                    "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4},
                },
                "screenshot": "/tmp/window.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/bundle",
            },
            "interaction": {
                "mode": "desktop-event",
                "click": {"screen_point": {"x": 10.5, "y": 20.25}},
            },
        }

        self.assertEqual(
            self.mod.desktop_action_success_lines("smoke", "mac", manifest),
            [
                "Desktop smoke PASS for `mac`",
                "  label: demo",
                "  pid: 123",
                "  before_screenshot: /tmp/before.png",
                "  diff_screenshot: /tmp/diff.png",
                "  image_change: changed=True method=pillow",
                "  image_change_bbox: 1,2 -> 3,4",
                "  screenshot: /tmp/window.png",
                "  ui_snapshot: /tmp/ui-tree.json",
                "  interaction_mode: desktop-event",
                "  click_screen_point: 10.5,20.25",
                "  bundle: /tmp/bundle",
            ],
        )

    def test_click_success_lines_skip_optional_values_when_absent(self) -> None:
        manifest = {
            "label": "demo",
            "pid": None,
            "artifacts": {
                "screenshot": "/tmp/window.png",
                "bundle_dir": "/tmp/bundle",
            },
            "interaction": {"click": {"screen_point": {}}},
        }

        self.assertEqual(
            self.mod.desktop_action_success_lines("click", "windows", manifest),
            [
                "Desktop click PASS for `windows`",
                "  label: demo",
                "  pid: None",
                "  screenshot: /tmp/window.png",
                "  bundle: /tmp/bundle",
            ],
        )

    def test_inspect_success_lines_keep_existing_short_shape(self) -> None:
        manifest = {
            "label": "inspect-demo",
            "pid": 456,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "diff_screenshot": "/tmp/diff.png",
                "image_change": {"changed": True, "method": "hash"},
                "screenshot": "/tmp/window.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/bundle",
            },
            "interaction": {
                "mode": "desktop-event",
                "click": {"screen_point": {"x": 1, "y": 2}},
            },
        }

        self.assertEqual(
            self.mod.desktop_action_success_lines("inspect", "ubuntu", manifest),
            [
                "Desktop inspect PASS for `ubuntu`",
                "  label: inspect-demo",
                "  pid: 456",
                "  screenshot: /tmp/window.png",
                "  ui_snapshot: /tmp/ui-tree.json",
                "  bundle: /tmp/bundle",
            ],
        )


if __name__ == "__main__":
    unittest.main()
