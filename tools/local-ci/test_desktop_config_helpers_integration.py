#!/usr/bin/env python3
"""Facade-level desktop config helper integration tests."""

from __future__ import annotations

import pathlib
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_config_helpers_integration",
        add_module_dir=True,
    )


class DesktopConfigHelpersIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_config_bundle_and_desktop_helper_fallback_edges(self) -> None:
        explicit_config = {"targets": {"mac": {"type": "local"}}}
        self.assertIs(self.mod.config_for_bundle_probe({}, explicit_config), explicit_config)
        with mock.patch.object(self.mod, "load_optional_config", return_value={"targets": {"ubuntu": {}}}):
            self.assertEqual(self.mod.config_for_bundle_probe({})["targets"], {"ubuntu": {}})
        with mock.patch.object(self.mod, "load_optional_config", return_value=None):
            self.assertEqual(self.mod.config_for_bundle_probe({}), {"targets": {}})

        self.assertIsNone(self.mod.target_name_for_ssh_host({"targets": {}}, "ubuntu"))
        self.assertTrue(
            self.mod.ssh_host_uses_windows_shell(
                {"targets": {"custom": {"host": "ssh-host", "repo_path": r"C:\Pulp"}}},
                "ssh-host",
            )
        )
        self.assertFalse(
            self.mod.ssh_host_uses_windows_shell(
                {"targets": {"ubuntu": {"host": "ubuntu", "repo_path": "/tmp/pulp"}}},
                "ubuntu",
            )
        )

        optional_caps = self.mod.desktop_capabilities_for(
            "macos-local",
            "v3",
            {
                "webview_driver": True,
                "debug_attach": True,
                "video_capture": True,
                "frame_stats": True,
            },
        )
        for capability in (
            "pulp_app_automation",
            "type_text",
            "webview_dom",
            "debug_command",
            "video_capture",
            "frame_stats",
        ):
            self.assertIn(capability, optional_caps)
        self.assertEqual(self.mod.desktop_capabilities_for("linux-xvfb", "v2").count("coordinate_click"), 1)
        self.assertEqual(
            self.mod.default_windows_session_task_name(" win target! "),
            "PulpDesktopAutomationAgent-win-target",
        )
        config = {}
        self.mod.update_target_repo_path(config, "windows", r"C:\Pulp")
        self.assertEqual(config["targets"]["windows"]["repo_path"], r"C:\Pulp")
        self.assertEqual(config["desktop_automation"]["targets"]["windows"]["repo_path"], r"C:\Pulp")


if __name__ == "__main__":
    unittest.main()
