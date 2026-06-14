#!/usr/bin/env python3
"""Desktop and bundle helper integration tests."""

from __future__ import annotations

import pathlib
import subprocess
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_basic_helpers_integration",
        add_module_dir=True,
    )


class DesktopBasicHelperIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_desktop_adapter_defaults_cover_fallbacks(self) -> None:
        self.assertEqual(self.mod.infer_desktop_adapter("mac", {"type": "local"}), "macos-local")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {"type": "local"}), "local-window")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {"type": "ssh"}), "remote-session-agent")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {}), "unknown")
        self.assertEqual(self.mod.default_desktop_bootstrap("custom"), "manual")
        self.assertEqual(self.mod.default_desktop_capability_tier("custom"), "v1")

    def test_probe_uploaded_bundle_size_handles_outputs(self) -> None:
        config = {"targets": {"windows": {"host": "win", "repo_path": r"C:\\Pulp"}}}
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="noise\n4096\n", stderr=""),
        ) as run:
            self.assertEqual(
                self.mod.probe_uploaded_bundle_size("win", "bundle.git", config=config),
                4096,
            )
            self.assertIn("cmd /V:OFF", run.call_args.args[0][-1])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="not-a-number\n", stderr=""),
        ):
            self.assertIsNone(
                self.mod.probe_uploaded_bundle_size("ubuntu", "bundle.git", config={"targets": {}})
            )

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="failed"),
        ):
            self.assertIsNone(
                self.mod.probe_uploaded_bundle_size("ubuntu", "bundle.git", config={"targets": {}})
            )


if __name__ == "__main__":
    unittest.main()
