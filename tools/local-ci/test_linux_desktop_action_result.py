#!/usr/bin/env python3
"""No-network tests for Linux desktop action result helpers."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("linux_desktop_action_result.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionResultTests(unittest.TestCase):
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

    def test_fetch_outputs_cleans_remote_bundle(self) -> None:
        fetched = []
        cleaned = []

        def fetch(_host, remote_path, local_path, **kwargs):
            fetched.append((remote_path, local_path.name, kwargs))
            local_path.parent.mkdir(parents=True, exist_ok=True)
            local_path.write_text("payload")
            return True

        self.mod.fetch_linux_remote_action_outputs(
            host="ubuntu-host",
            remote_bundle_copy_root="~/.local/state/pulp/desktop/bundle",
            remote_bundle_cleanup_expr='"$HOME/.local/state/pulp/desktop/bundle"',
            pulp_app_automation=True,
            capture_before=True,
            capture_ui_snapshot=True,
            fetch_ssh_artifact_fn=fetch,
            cleanup_remote_ssh_dir_fn=lambda host, expr: cleaned.append((host, expr)),
            **{key: value for key, value in self.paths.items() if key != "diff_screenshot_path"},
        )

        self.assertIn(("~/.local/state/pulp/desktop/bundle/screenshots/window.png", "window.png", {}), fetched)
        self.assertIn(("~/.local/state/pulp/desktop/bundle/screenshots/before.png", "before.png", {"optional": False}), fetched)
        self.assertIn(("~/.local/state/pulp/desktop/bundle/ui-tree.json", "ui-tree.json", {}), fetched)
        self.assertEqual(cleaned, [("ubuntu-host", '"$HOME/.local/state/pulp/desktop/bundle"')])

    def test_read_pid_ignores_missing_or_invalid_values(self) -> None:
        self.assertIsNone(self.mod.read_linux_pid_file(self.paths["pid_path"]))
        self.paths["pid_path"].write_text("not-a-pid")
        self.assertIsNone(self.mod.read_linux_pid_file(self.paths["pid_path"]))
        self.paths["pid_path"].write_text("4242")
        self.assertEqual(self.mod.read_linux_pid_file(self.paths["pid_path"]), 4242)

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
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))
        self.assertEqual(manifest["inspector"], {"root_type": "Window"})
        self.assertEqual(manifest["interaction"]["selector"]["click_view_id"], "bypass")

    def test_build_manifest_adds_x11_window_driver_click_summary(self) -> None:
        manifest = self.mod.build_linux_desktop_action_manifest(
            target_name="ubuntu",
            target={"adapter": "linux-xvfb"},
            command="/usr/bin/xcalc",
            launch_command="/usr/bin/xcalc",
            host="ubuntu-host",
            repo_path="/repo",
            action_name="click",
            label="calculator",
            started_at="start",
            completed_at="done",
            bundle_dir=self.root,
            remote_bundle_copy_root="~/bundle",
            capture_before=False,
            capture_ui_snapshot=False,
            interaction_requested=True,
            pulp_app_automation=False,
            click_point="20,30",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            default_desktop_label_fn=lambda command: Path(command or "").name,
            image_change_summary_fn=lambda *_args, **_kwargs: {},
            parse_coordinate_pair_fn=lambda point, **_kwargs: tuple(float(part) for part in point.split(",")),
            view_tree_inspector_summary_fn=lambda _tree: {},
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
            **self.paths,
        )

        self.assertEqual(manifest["interaction"]["mode"], "x11-window-driver")
        self.assertEqual(manifest["interaction"]["click"]["point"], "20,30")
        self.assertEqual(manifest["interaction"]["click"]["content_point"], {"x": 20.0, "y": 30.0})


if __name__ == "__main__":
    unittest.main()
