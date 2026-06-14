#!/usr/bin/env python3
"""Tests for desktop status command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_status_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopStatusCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_STATUS_COMMAND_EXPORTS, ("cmd_desktop_status",))

    def test_status_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_status=runner),
        }
        deps = {
            "load_config_fn": object(),
            "desktop_receipt_for_fn": object(),
            "desktop_capabilities_for_fn": object(),
            "desktop_optional_capabilities_fn": object(),
            "desktop_run_manifests_fn": object(),
            "desktop_run_summary_fn": object(),
            "desktop_proof_summaries_fn": object(),
            "normalize_desktop_optional_config_fn": object(),
            "desktop_target_contract_fn": object(),
            "desktop_publish_reports_fn": object(),
            "desktop_status_lines_fn": object(),
            "short_sha_fn": object(),
            "windows_tooling_detail_fn": object(),
            "windows_repo_checkout_detail_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "desktop_status_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_status(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

if __name__ == "__main__":
    unittest.main()
