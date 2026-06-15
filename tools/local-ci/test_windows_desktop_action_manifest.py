#!/usr/bin/env python3
"""No-network tests for Windows desktop action manifest assembly."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_desktop_action_manifest.py", add_module_dir=True)


class WindowsDesktopActionManifestTests(unittest.TestCase):
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
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_build_manifest_adds_artifacts_inspector_and_interaction(self) -> None:
        for path in [self.paths["screenshot_path"], self.paths["before_screenshot_path"]]:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"png")
        self.paths["ui_snapshot_path"].write_text(json.dumps({"id": "root", "type": "Window"}))

        def image_change(_before_path, _after_path, *, diff_output_path=None):
            diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        manifest = self.mod.build_windows_desktop_action_manifest(
            target_name="windows",
            target={"adapter": "windows-session-agent", "repo_path": r"C:\Pulp"},
            command=r"C:\Pulp\build\ui-preview.exe",
            launch_command=r"C:\Prepared\ui-preview.exe",
            host="win-host",
            action_name="inspect",
            label=None,
            started_at="2026-06-11T00:00:00+00:00",
            completed_at="2026-06-11T00:00:01+00:00",
            remote_manifest={"status": "pass", "pid": 5153, "window": {"title": "UI Preview"}},
            bundle_dir=self.root,
            agent_manifest_path=self.root / "agent-manifest.json",
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
            view_tree_inspector_summary_fn=lambda tree: {"root_type": tree["type"]},
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
            **self.paths,
        )

        self.assertEqual(manifest["label"], "C:\\Pulp\\build\\ui-preview.exe")
        self.assertEqual(manifest["command"], r"C:\Prepared\ui-preview.exe")
        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["artifacts"]["image_change"], {"changed": True})
        self.assertEqual(manifest["inspector"], {"root_type": "Window"})
        self.assertEqual(manifest["interaction"]["selector"]["click_view_id"], "bypass")


if __name__ == "__main__":
    unittest.main()
