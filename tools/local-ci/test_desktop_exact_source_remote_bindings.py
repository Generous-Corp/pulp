#!/usr/bin/env python3
"""Tests for remote exact-source preparation compatibility bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_remote_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceRemoteBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_remote_facade_reexports_focused_helpers(self):
        self.assertEqual(self.mod.prepare_linux_exact_sha_source.__module__, "desktop_exact_source_linux_bindings")
        self.assertEqual(self.mod.prepare_windows_exact_sha_source.__module__, "desktop_exact_source_windows_bindings")

    def test_remote_exports_compose_platform_groups(self):
        expected = (
            *self.mod.DESKTOP_EXACT_SOURCE_LINUX_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_remote_installer_routes_selected_platform_helpers(self):
        bindings = {
            "_source_prep": types.SimpleNamespace(
                prepare_linux_exact_sha_source=lambda *args, **kwargs: {"platform": "linux"},
                prepare_windows_exact_sha_source=lambda *args, **kwargs: {"platform": "windows"},
            ),
            "ROOT": Path("/repo"),
            "subprocess": types.SimpleNamespace(run=object()),
            "sync_job_bundle_to_ssh_host": object(),
            "git_origin_clone_url": object(),
            "desktop_source_cache_key": object(),
            "fetch_ssh_artifact": object(),
            "rewrite_launch_command_for_posix_root": object(),
            "ps_literal": object(),
            "windows_contract_expand_expression": object(),
            "split_windows_prepare_commands": object(),
            "validate_windows_prepare_commands": object(),
            "run_windows_ssh_powershell": object(),
            "windows_ssh_fetch_file": object(),
            "rewrite_launch_command_for_windows_root": object(),
        }

        self.mod.install_desktop_exact_source_remote_helpers(bindings, ("prepare_linux_exact_sha_source",))

        self.assertEqual(
            bindings["prepare_linux_exact_sha_source"](Path("/bundle"), "ubuntu", "host", "./tool", {"sha": "abc123"}),
            {"platform": "linux"},
        )
        self.assertNotIn("prepare_windows_exact_sha_source", bindings)


if __name__ == "__main__":
    unittest.main()
