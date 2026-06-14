#!/usr/bin/env python3
"""Tests for Windows remote exact-source preparation dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_windows_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceWindowsBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")

    def test_prepare_windows_exact_sha_source_binds_facade_dependencies(self):
        captured = {}

        def prepare(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"platform": "windows"}

        bindings = {
            "_source_prep": types.SimpleNamespace(prepare_windows_exact_sha_source=prepare),
            "ROOT": self.root,
            "sync_job_bundle_to_ssh_host": object(),
            "git_origin_clone_url": object(),
            "desktop_source_cache_key": object(),
            "ps_literal": object(),
            "windows_contract_expand_expression": object(),
            "split_windows_prepare_commands": object(),
            "validate_windows_prepare_commands": object(),
            "run_windows_ssh_powershell": object(),
            "windows_ssh_fetch_file": object(),
            "rewrite_launch_command_for_windows_root": object(),
        }

        result = self.mod.prepare_windows_exact_sha_source(
            bindings,
            Path("/bundle"),
            "windows",
            "host",
            r".\tool.exe",
            {"sha": "abc123"},
        )

        self.assertEqual(result, {"platform": "windows"})
        self.assertEqual(captured["args"], (Path("/bundle"), "windows", "host", r".\tool.exe", {"sha": "abc123"}))
        self.assertIs(captured["kwargs"]["sync_job_bundle_to_ssh_host_fn"], bindings["sync_job_bundle_to_ssh_host"])
        self.assertIs(captured["kwargs"]["git_origin_clone_url_fn"], bindings["git_origin_clone_url"])
        self.assertIs(captured["kwargs"]["desktop_source_cache_key_fn"], bindings["desktop_source_cache_key"])
        self.assertEqual(captured["kwargs"]["root"], self.root)
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])
        self.assertIs(captured["kwargs"]["windows_contract_expand_expression_fn"], bindings["windows_contract_expand_expression"])
        self.assertIs(captured["kwargs"]["split_windows_prepare_commands_fn"], bindings["split_windows_prepare_commands"])
        self.assertIs(captured["kwargs"]["validate_windows_prepare_commands_fn"], bindings["validate_windows_prepare_commands"])
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["windows_ssh_fetch_file_fn"], bindings["windows_ssh_fetch_file"])
        self.assertIs(captured["kwargs"]["rewrite_launch_command_for_windows_root_fn"], bindings["rewrite_launch_command_for_windows_root"])

    def test_windows_exports_and_installer_wire_named_helper(self):
        expected = ("prepare_windows_exact_sha_source",)
        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS, expected)

        bindings = {
            "_source_prep": types.SimpleNamespace(prepare_windows_exact_sha_source=lambda *args, **kwargs: {"platform": "windows"}),
            "ROOT": self.root,
            "sync_job_bundle_to_ssh_host": object(),
            "git_origin_clone_url": object(),
            "desktop_source_cache_key": object(),
            "ps_literal": object(),
            "windows_contract_expand_expression": object(),
            "split_windows_prepare_commands": object(),
            "validate_windows_prepare_commands": object(),
            "run_windows_ssh_powershell": object(),
            "windows_ssh_fetch_file": object(),
            "rewrite_launch_command_for_windows_root": object(),
        }

        self.mod.install_desktop_exact_source_windows_helpers(bindings)

        self.assertEqual(
            bindings["prepare_windows_exact_sha_source"](Path("/bundle"), "windows", "host", r".\tool.exe", {"sha": "abc123"}),
            {"platform": "windows"},
        )


if __name__ == "__main__":
    unittest.main()
