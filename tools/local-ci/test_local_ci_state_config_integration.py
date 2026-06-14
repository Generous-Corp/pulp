#!/usr/bin/env python3
"""Facade-level local-ci state path and config normalizer integration tests."""

from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_state_config_integration", add_module_dir=True)


class LocalCiStateConfigIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_state_dir_uses_xdg_and_home_fallbacks_on_non_macos(self) -> None:
        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.object(self.mod.sys, "platform", "darwin"):
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "home" / "Library" / "Application Support" / "Pulp" / "local-ci",
                    )

        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.object(self.mod.sys, "platform", "linux"):
                with mock.patch.dict(os.environ, {"XDG_STATE_HOME": str(self.root / "xdg")}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "xdg" / "pulp" / "local-ci",
                    )
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "home" / ".local" / "state" / "pulp" / "local-ci",
                    )

    def test_state_path_helpers_create_expected_directories_and_logs(self) -> None:
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            self.mod.ensure_state_dirs()
            for path in (
                self.mod.results_dir(),
                self.mod.cloud_runs_dir(),
                self.mod.logs_dir(),
                self.mod.bundles_dir(),
                self.mod.desktop_state_dir(),
                self.mod.desktop_receipts_dir(),
            ):
                self.assertTrue(path.is_dir(), f"{path} should exist")

            log_path = self.mod.prepare_target_log("job-1", "mac")
            self.assertEqual(log_path, self.mod.target_log_path("job-1", "mac"))
            self.assertTrue(log_path.is_file())
            self.assertEqual(log_path.read_text(), "")

    def test_default_desktop_artifact_root_platform_branches(self) -> None:
        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.dict(os.environ, {"PULP_DESKTOP_ARTIFACT_ROOT": str(self.root / "override")}, clear=True):
                self.assertEqual(self.mod.default_desktop_artifact_root(), self.root / "override")

            with mock.patch.object(self.mod.sys, "platform", "darwin"):
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "home" / "Library" / "Application Support" / "Pulp" / "desktop-automation" / "runs",
                    )

            with mock.patch.object(self.mod.sys, "platform", "win32"):
                with mock.patch.dict(os.environ, {"LOCALAPPDATA": str(self.root / "localapp")}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "localapp" / "Pulp" / "desktop-automation" / "runs",
                    )

            with mock.patch.object(self.mod.sys, "platform", "linux"):
                with mock.patch.dict(os.environ, {"XDG_STATE_HOME": str(self.root / "xdg")}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "xdg" / "pulp" / "desktop-automation" / "runs",
                    )
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "home" / ".local" / "state" / "pulp" / "desktop-automation" / "runs",
                    )

    def test_normalizers_reject_invalid_modes_and_parse_booleans(self) -> None:
        self.assertEqual(self.mod.normalize_validation_mode(" SMOKE "), "smoke")
        with self.assertRaisesRegex(ValueError, "Invalid validation mode"):
            self.mod.normalize_validation_mode("quick")

        self.assertEqual(self.mod.normalize_desktop_source_mode("exact_sha"), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop source mode"):
            self.mod.normalize_desktop_source_mode("snapshot")

        self.assertEqual(self.mod.normalize_publish_mode("issue-comment"), "issue-comment")
        with self.assertRaisesRegex(ValueError, "Invalid desktop publish mode"):
            self.mod.normalize_publish_mode("slack")

        for value in (True, 1, 0.5, "yes", "ON"):
            self.assertTrue(self.mod.parse_config_bool(value))
        for value in (False, 0, "", "no", "off"):
            self.assertFalse(self.mod.parse_config_bool(value))
        with self.assertRaisesRegex(ValueError, "Invalid boolean value"):
            self.mod.parse_config_bool("maybe")


if __name__ == "__main__":
    unittest.main()
