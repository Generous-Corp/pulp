#!/usr/bin/env python3
"""No-network tests for Linux desktop action manifest assembly."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("linux_desktop_action_manifest.py", add_module_dir=True)


class LinuxDesktopActionManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.paths = {
            "screenshot_path": self.root / "screenshots" / "window.png",
            "before_screenshot_path": self.root / "screenshots" / "before.png",
            "diff_screenshot_path": self.root / "screenshots" / "diff.png",
            "ui_snapshot_path": self.root / "ui-tree.json",
            "log_path": self.root / "stdout.log",
            "err_path": self.root / "stderr.log",
            "pid_path": self.root / "pid.txt",
            "window_id_path": self.root / "window-id.txt",
            "window_title_path": self.root / "window-title.txt",
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_build_manifest_adds_metadata_snapshot_diff_and_pulp_interaction(self) -> None:
        self.paths["screenshot_path"].parent.mkdir(parents=True, exist_ok=True)
        self.paths["screenshot_path"].write_bytes(b"png")
        self.paths["before_screenshot_path"].write_bytes(b"before")
        self.paths["pid_path"].write_text("5151")
        self.paths["window_id_path"].write_text("0x123")
        self.paths["window_title_path"].write_text("UI Preview")
        self.paths["ui_snapshot_path"].write_text(json.dumps({"id": "root", "type": "Window"}))

        def image_change(_before_path, _after_path, *, diff_output_path=None):
            diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        manifest = self.mod.build_linux_desktop_action_manifest(
            target_name="ubuntu",
            target={"adapter": "linux-xvfb"},
            command="/repo/build/ui-preview",
            launch_command="/prepared/ui-preview",
            host="ubuntu-host",
            repo_path="/repo",
            action_name="click",
            label=None,
            started_at="start",
            completed_at="done",
            bundle_dir=self.root,
            remote_bundle_copy_root="~/bundle",
            capture_before=True,
            capture_ui_snapshot=True,
            interaction_requested=True,
            pulp_app_automation=True,
            click_point=None,
            click_view_id="bypass",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            default_desktop_label_fn=lambda command: Path(command or "").name,
            image_change_summary_fn=image_change,
            parse_coordinate_pair_fn=lambda *_args, **_kwargs: self.fail("unexpected parse"),
            view_tree_inspector_summary_fn=lambda tree: {"root_type": tree["type"]},
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
            **self.paths,
        )

        self.assertEqual(manifest["pid"], 5151)
        self.assertEqual(manifest["label"], "ui-preview")
        self.assertEqual(manifest["window"], {"window_id": "0x123", "title": "UI Preview"})
        self.assertEqual(manifest["artifacts"]["image_change"], {"changed": True})
        self.assertEqual(manifest["inspector"], {"root_type": "Window"})
        self.assertEqual(manifest["interaction"]["selector"]["click_view_id"], "bypass")


if __name__ == "__main__":
    unittest.main()
