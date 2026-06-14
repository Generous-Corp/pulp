#!/usr/bin/env python3
"""No-network tests for Linux desktop action artifact manifest helpers."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("linux_desktop_action_artifacts.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionArtifactsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_attach_before_diff_and_ui_snapshot_artifacts(self) -> None:
        screenshot_path = self.root / "screenshots" / "window.png"
        before_screenshot_path = self.root / "screenshots" / "before.png"
        diff_screenshot_path = self.root / "screenshots" / "diff.png"
        ui_snapshot_path = self.root / "ui-tree.json"
        screenshot_path.parent.mkdir(parents=True)
        screenshot_path.write_bytes(b"png")
        before_screenshot_path.write_bytes(b"before")
        ui_snapshot_path.write_text(json.dumps({"id": "root", "type": "Window"}))
        manifest = {"artifacts": {}}

        def image_change(_before_path, _after_path, *, diff_output_path=None):
            diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        self.mod.attach_linux_before_diff_artifacts(
            manifest,
            capture_before=True,
            before_screenshot_path=before_screenshot_path,
            screenshot_path=screenshot_path,
            diff_screenshot_path=diff_screenshot_path,
            image_change_summary_fn=image_change,
        )
        self.mod.attach_linux_ui_snapshot(
            manifest,
            capture_ui_snapshot=True,
            ui_snapshot_path=ui_snapshot_path,
            view_tree_inspector_summary_fn=lambda tree: {"root_type": tree["type"]},
        )

        self.assertEqual(manifest["artifacts"]["image_change"], {"changed": True})
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))
        self.assertEqual(manifest["inspector"], {"root_type": "Window"})


if __name__ == "__main__":
    unittest.main()
