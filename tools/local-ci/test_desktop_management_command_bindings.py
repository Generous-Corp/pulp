#!/usr/bin/env python3
"""Tests for desktop management command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_management_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopManagementCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_STATUS_COMMAND_EXPORTS,
            *self.mod.DESKTOP_CONFIG_COMMAND_EXPORTS,
            *self.mod.DESKTOP_REPORT_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_MANAGEMENT_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def bindings(self, runner_name: str, runner):
        desktop_cli = types.SimpleNamespace(desktop_recent_lines=object())
        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": desktop_cli,
        }
        for name in [
            "load_config",
            "desktop_run_manifests",
            "desktop_run_summary",
            "short_sha",
        ]:
            bindings[name] = object()
        return bindings

    def test_recent_binds_management_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 4

        bindings = self.bindings("cmd_desktop_recent", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_recent(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["desktop_recent_lines_fn"], bindings["_desktop_cli"].desktop_recent_lines)

    def test_install_desktop_management_command_helpers_routes_each_group(self) -> None:
        bindings = self.bindings("cmd_desktop_recent", lambda *args, **kwargs: 41)
        bindings["_desktop_commands_cli"].cmd_desktop_status = lambda *args, **kwargs: 43
        bindings["_desktop_commands_cli"].cmd_desktop_config_show = lambda *args, **kwargs: 47
        bindings["_desktop_cli"].desktop_status_lines = object()
        bindings["_desktop_cli"].desktop_config_show_lines = object()
        for name in [
            "desktop_receipt_for",
            "desktop_capabilities_for",
            "desktop_optional_capabilities",
            "desktop_proof_summaries",
            "normalize_desktop_optional_config",
            "desktop_target_contract",
            "desktop_publish_reports",
            "windows_tooling_detail",
            "windows_repo_checkout_detail",
        ]:
            bindings[name] = object()

        self.mod.install_desktop_management_command_helpers(
            bindings,
            ("cmd_desktop_status", "cmd_desktop_config_show", "cmd_desktop_recent"),
        )

        self.assertEqual(bindings["cmd_desktop_status"](object()), 43)
        self.assertEqual(bindings["cmd_desktop_config_show"](object()), 47)
        self.assertEqual(bindings["cmd_desktop_recent"](object()), 41)
        self.assertNotIn("cmd_desktop_cleanup", bindings)

    def test_install_desktop_management_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_management_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_management_command_helpers(bindings, ("future_desktop_management_command_helper",))

        self.assertEqual(bindings["future_desktop_management_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
