#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("macos_desktop_action_env.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopActionEnvTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self):
        self.tmpdir.cleanup()

    def paths(self):
        return {
            "ui_snapshot_path": self.root / "ui-tree.json",
            "before_screenshot_path": self.root / "before.png",
            "screenshot_path": self.root / "after.png",
        }

    def test_capture_env_sets_view_tree_only(self):
        env = {"EXISTING": "1"}

        self.mod.apply_macos_direct_launch_env(
            env,
            capture_ui_snapshot=True,
            use_pulp_app_automation=False,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.25,
            **self.paths(),
        )

        self.assertEqual(env["EXISTING"], "1")
        self.assertEqual(env["PULP_VIEW_TREE_OUT"], str(self.root / "ui-tree.json"))
        self.assertNotIn("PULP_AUTOMATION_AFTER_OUT", env)

    def test_pulp_app_automation_env_sets_selector_and_artifact_paths(self):
        env = {}

        self.mod.apply_macos_direct_launch_env(
            env,
            capture_ui_snapshot=True,
            use_pulp_app_automation=True,
            click_point="10,20",
            click_view_id="bypass",
            click_view_type="Toggle",
            click_view_text="Bypass",
            click_view_label="Bypass toggle",
            capture_before=True,
            settle_secs=1.25,
            **self.paths(),
        )

        self.assertEqual(env["PULP_VIEW_TREE_OUT"], str(self.root / "ui-tree.json"))
        self.assertEqual(env["PULP_AUTOMATION_CLICK_POINT"], "10,20")
        self.assertEqual(env["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass")
        self.assertEqual(env["PULP_AUTOMATION_CLICK_VIEW_TYPE"], "Toggle")
        self.assertEqual(env["PULP_AUTOMATION_CLICK_VIEW_TEXT"], "Bypass")
        self.assertEqual(env["PULP_AUTOMATION_CLICK_VIEW_LABEL"], "Bypass toggle")
        self.assertEqual(env["PULP_AUTOMATION_BEFORE_OUT"], str(self.root / "before.png"))
        self.assertEqual(env["PULP_AUTOMATION_AFTER_OUT"], str(self.root / "after.png"))
        self.assertEqual(env["PULP_AUTOMATION_DELAY_MS"], "1000")
        self.assertEqual(env["PULP_AUTOMATION_AFTER_DELAY_MS"], "1250")
        self.assertEqual(env["PULP_AUTOMATION_EXIT_AFTER"], "1")

    def test_pulp_app_automation_delay_clamps_negative_settle_time(self):
        env = {}

        self.mod.apply_macos_pulp_app_automation_env(
            env,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=-1.0,
            before_screenshot_path=self.root / "before.png",
            screenshot_path=self.root / "after.png",
        )

        self.assertNotIn("PULP_AUTOMATION_BEFORE_OUT", env)
        self.assertEqual(env["PULP_AUTOMATION_AFTER_DELAY_MS"], "0")
        self.assertEqual(env["PULP_AUTOMATION_AFTER_OUT"], str(self.root / "after.png"))


if __name__ == "__main__":
    unittest.main()
