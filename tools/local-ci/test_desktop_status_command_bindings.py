#!/usr/bin/env python3
"""Tests for desktop status command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_status_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopStatusCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_STATUS_COMMAND_EXPORTS, ("cmd_desktop_status",))

    def test_status_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_status=runner),
            "_desktop_cli": types.SimpleNamespace(desktop_status_lines=object()),
        }
        for name in [
            "load_config",
            "desktop_receipt_for",
            "desktop_capabilities_for",
            "desktop_optional_capabilities",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "normalize_desktop_optional_config",
            "desktop_target_contract",
            "desktop_publish_reports",
            "short_sha",
            "windows_tooling_detail",
            "windows_repo_checkout_detail",
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_status(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "load_config",
            "desktop_receipt_for",
            "desktop_capabilities_for",
            "desktop_optional_capabilities",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "normalize_desktop_optional_config",
            "desktop_target_contract",
            "desktop_publish_reports",
            "short_sha",
            "windows_tooling_detail",
            "windows_repo_checkout_detail",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
        self.assertIs(captured["kwargs"]["desktop_status_lines_fn"], bindings["_desktop_cli"].desktop_status_lines)

    def test_install_desktop_status_command_helpers_wires_named_exports(self) -> None:
        def runner(*args, **kwargs):
            return 17

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_status=runner),
            "_desktop_cli": types.SimpleNamespace(desktop_status_lines=object()),
        }
        for name in [
            "load_config",
            "desktop_receipt_for",
            "desktop_capabilities_for",
            "desktop_optional_capabilities",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "normalize_desktop_optional_config",
            "desktop_target_contract",
            "desktop_publish_reports",
            "short_sha",
            "windows_tooling_detail",
            "windows_repo_checkout_detail",
        ]:
            bindings[name] = object()

        self.mod.install_desktop_status_command_helpers(bindings, ("cmd_desktop_status",))

        self.assertEqual(bindings["cmd_desktop_status"](object()), 17)

    def test_install_desktop_status_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_status_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_status_command_helpers(bindings, ("future_desktop_status_command_helper",))

        self.assertEqual(bindings["future_desktop_status_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
