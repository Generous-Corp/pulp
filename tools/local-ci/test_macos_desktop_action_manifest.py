#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("macos_desktop_action_manifest.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopActionManifestTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self):
        self.tmpdir.cleanup()

    def paths(self):
        return {
            "bundle_dir": self.root / "bundle",
            "screenshot_path": self.root / "window.png",
            "before_screenshot_path": self.root / "before.png",
            "diff_screenshot_path": self.root / "diff.png",
            "ui_snapshot_path": self.root / "ui-tree.json",
            "log_path": self.root / "stdout.log",
            "err_path": self.root / "stderr.log",
        }

    def base_paths(self):
        paths = self.paths()
        return {
            key: paths[key]
            for key in (
                "bundle_dir",
                "screenshot_path",
                "log_path",
                "err_path",
            )
        }

    def test_action_label_prefers_label_then_bundle_then_command_stem(self):
        self.assertEqual(self.mod.macos_action_label(label="explicit", bundle_id="bundle", launch_command="/tmp/app"), "explicit")
        self.assertEqual(self.mod.macos_action_label(label=None, bundle_id="com.example.App", launch_command="/tmp/app"), "com.example.App")
        self.assertEqual(self.mod.macos_action_label(label=None, bundle_id=None, launch_command="/tmp/ui-preview --flag"), "ui-preview")

    def test_base_manifest_includes_launch_descriptor_and_artifacts(self):
        manifest = self.mod.macos_action_base_manifest(
            action_name="inspect",
            label=None,
            bundle_id=None,
            launch_command="/tmp/ui-preview",
            pid=4242,
            started_at="start",
            completed_at="done",
            window={"windowId": 88},
            launch_descriptor={"command": ["/tmp/ui-preview"]},
            **self.base_paths(),
        )

        self.assertEqual(manifest["target"], "mac")
        self.assertEqual(manifest["adapter"], "macos-local")
        self.assertEqual(manifest["label"], "ui-preview")
        self.assertEqual(manifest["command"], ["/tmp/ui-preview"])
        self.assertEqual(manifest["artifacts"]["bundle_dir"], str(self.root / "bundle"))
        self.assertEqual(manifest["artifacts"]["stdout"], str(self.root / "stdout.log"))

    def test_full_manifest_attaches_before_diff_inspector_and_interaction(self):
        paths = self.paths()
        paths["before_screenshot_path"].write_bytes(b"before")
        paths["screenshot_path"].write_bytes(b"after")

        def image_change(_before, _after, *, diff_output_path):
            diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        manifest = self.mod.build_macos_action_manifest(
            action_name="click",
            label="ui-preview",
            bundle_id=None,
            launch_command="/tmp/ui-preview",
            pid=4242,
            started_at="start",
            completed_at="done",
            window={"windowId": 88},
            launch_descriptor={"command": ["/tmp/ui-preview"]},
            capture_before=True,
            interaction_requested=True,
            inspector_summary={"node_count": 2},
            interaction_summary={"mode": "desktop-event"},
            image_change_summary_fn=image_change,
            **paths,
        )

        self.assertEqual(manifest["artifacts"]["before_screenshot"], str(paths["before_screenshot_path"]))
        self.assertEqual(manifest["artifacts"]["image_change"], {"changed": True})
        self.assertEqual(manifest["artifacts"]["diff_screenshot"], str(paths["diff_screenshot_path"]))
        self.assertEqual(manifest["artifacts"]["ui_snapshot"], str(paths["ui_snapshot_path"]))
        self.assertEqual(manifest["inspector"], {"node_count": 2})
        self.assertEqual(manifest["interaction"], {"mode": "desktop-event"})

    def test_before_artifact_is_recorded_without_diff_when_screenshots_missing(self):
        manifest = self.mod.build_macos_action_manifest(
            action_name="click",
            label="ui-preview",
            bundle_id=None,
            launch_command="/tmp/ui-preview",
            pid=None,
            started_at="start",
            completed_at="done",
            window={"windowId": 88},
            launch_descriptor={"command": ["/tmp/ui-preview"]},
            capture_before=True,
            interaction_requested=True,
            inspector_summary=None,
            interaction_summary=None,
            image_change_summary_fn=lambda *_args, **_kwargs: {"changed": True},
            **self.paths(),
        )

        self.assertIn("before_screenshot", manifest["artifacts"])
        self.assertNotIn("image_change", manifest["artifacts"])
        self.assertNotIn("diff_screenshot", manifest["artifacts"])


if __name__ == "__main__":
    unittest.main()
