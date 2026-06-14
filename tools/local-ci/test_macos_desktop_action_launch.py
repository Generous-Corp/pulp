#!/usr/bin/env python3
"""No-network tests for macOS desktop action launch helpers."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("macos_desktop_action_launch.py")


class FakeProcess:
    def __init__(self, pid: int = 4242) -> None:
        self.pid = pid


class MacosDesktopActionLaunchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.paths = {
            "ui_snapshot_path": self.root / "ui-tree.json",
            "before_screenshot_path": self.root / "before.png",
            "screenshot_path": self.root / "after.png",
            "log_path": self.root / "stdout.log",
            "err_path": self.root / "stderr.log",
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def launch(self, **overrides):
        opened = overrides.pop("opened", [])
        activated = overrides.pop("activated", [])
        quit_ids = overrides.pop("quit_ids", [])
        launched = overrides.pop("launched", [])
        slept = overrides.pop("slept", [])
        window = {"windowId": 88, "bounds": {"width": 100, "height": 100}}
        kwargs = {
            "bundle_id": None,
            "launch_command": "/repo/build/ui-preview --flag",
            "launch_cwd": None,
            "capture_ui_snapshot": False,
            "use_pulp_app_automation": False,
            "click_point": None,
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            "capture_before": False,
            "settle_secs": 0.0,
            "timeout_secs": 5.0,
            **self.paths,
            "quit_macos_bundle_id_fn": lambda bundle_id: quit_ids.append(bundle_id),
            "sleep_fn": lambda secs: slept.append(secs),
            "run_fn": lambda args, **run_kwargs: opened.append((args, run_kwargs)),
            "activate_macos_bundle_id_fn": lambda bundle_id: activated.append(bundle_id),
            "wait_for_macos_bundle_window_fn": lambda _bundle_id, _timeout: (5151, window),
            "split_command_fn": lambda command: command.split(),
            "detect_macos_app_bundle_fn": lambda _command: None,
            "macos_bundle_id_for_app_path_fn": lambda _path: None,
            "environ_copy_fn": lambda: {},
            "popen_fn": lambda args, **popen_kwargs: launched.append((args, popen_kwargs)) or FakeProcess(),
            "wait_for_macos_window_fn": lambda _pid, _timeout: window,
        }
        kwargs.update(overrides)
        result = self.mod.launch_macos_desktop_action(**kwargs)
        return result, opened, activated, quit_ids, launched, slept

    def test_launches_bundle_id_with_open_b(self) -> None:
        result, opened, activated, quit_ids, launched, slept = self.launch(bundle_id="com.example.Pulp")

        self.assertIsNone(result["proc"])
        self.assertEqual(result["pid"], 5151)
        self.assertEqual(result["launch_descriptor"], {"bundle_id": "com.example.Pulp"})
        self.assertEqual(opened[0][0], ["open", "-b", "com.example.Pulp"])
        self.assertEqual(activated, ["com.example.Pulp"])
        self.assertEqual(quit_ids, ["com.example.Pulp"])
        self.assertEqual(launched, [])
        self.assertEqual(slept, [0.2, 0.75, 0.75])

    def test_launches_app_bundle_with_inferred_bundle_id(self) -> None:
        app_path = self.root / "Pulp.app"
        result, opened, activated, quit_ids, launched, _slept = self.launch(
            launch_command=str(app_path),
            detect_macos_app_bundle_fn=lambda _command: app_path,
            macos_bundle_id_for_app_path_fn=lambda _path: "com.example.PulpApp",
        )

        self.assertIsNone(result["proc"])
        self.assertEqual(result["launch_descriptor"], {"bundle_id": "com.example.PulpApp", "app_path": str(app_path)})
        self.assertEqual(opened[0][0], ["open", "-a", str(app_path)])
        self.assertEqual(activated, ["com.example.PulpApp"])
        self.assertEqual(quit_ids, ["com.example.PulpApp"])
        self.assertEqual(launched, [])

    def test_launches_direct_command_with_env_and_cwd(self) -> None:
        result, _opened, _activated, _quit_ids, launched, _slept = self.launch(
            launch_cwd=str(self.root),
            capture_ui_snapshot=True,
            click_view_id="bypass-toggle",
            use_pulp_app_automation=True,
            capture_before=True,
            settle_secs=0.25,
        )

        self.assertEqual(result["proc"].pid, 4242)
        self.assertEqual(result["launch_descriptor"], {"command": ["/repo/build/ui-preview", "--flag"]})
        args, kwargs = launched[0]
        self.assertEqual(args, ["/repo/build/ui-preview", "--flag"])
        self.assertEqual(kwargs["cwd"], str(self.root))
        self.assertEqual(kwargs["env"]["PULP_VIEW_TREE_OUT"], str(self.paths["ui_snapshot_path"]))
        self.assertEqual(kwargs["env"]["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(kwargs["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "250")
        self.assertEqual(kwargs["env"]["PULP_AUTOMATION_BEFORE_OUT"], str(self.paths["before_screenshot_path"]))
        self.assertEqual(kwargs["env"]["PULP_AUTOMATION_AFTER_OUT"], str(self.paths["screenshot_path"]))

    def test_rejects_empty_command_without_bundle_id(self) -> None:
        with self.assertRaisesRegex(ValueError, "either --command or --bundle-id"):
            self.launch(launch_command="")


if __name__ == "__main__":
    unittest.main()
