#!/usr/bin/env python3
"""Tests for desktop setup command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_setup_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSetupCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_INSTALL_COMMAND_EXPORTS,
            *self.mod.DESKTOP_DOCTOR_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SETUP_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def bindings(self, runner_name: str, runner):
        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "ROOT": Path("/repo"),
            "subprocess": types.SimpleNamespace(run=object()),
            "uuid": types.SimpleNamespace(uuid4=lambda: types.SimpleNamespace(hex="abcdef1234567890")),
        }
        for name in [
            "load_config",
            "resolve_desktop_target",
            "_check_writable_dir",
            "desktop_target_contract",
            "ensure_host_reachable",
            "bootstrap_windows_session_agent",
            "probe_windows_session_agent",
            "sync_job_bundle_to_ssh_host",
            "ensure_windows_remote_tooling",
            "windows_remote_tooling_ready",
            "ensure_windows_remote_repo_checkout",
            "git_origin_clone_url",
            "windows_repo_checkout_ready",
            "update_target_repo_path",
            "save_config",
            "now_iso",
            "desktop_target_receipt_path",
            "atomic_write_text",
            "windows_tooling_detail",
            "desktop_doctor_checks",
        ]:
            bindings[name] = object()
        return bindings

    def test_install_binds_setup_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = self.bindings("cmd_desktop_install", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_install(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["check_writable_dir_fn"], bindings["_check_writable_dir"])
        self.assertIs(captured["kwargs"]["subprocess_run_fn"], bindings["subprocess"].run)
        self.assertEqual(captured["kwargs"]["new_install_job_id_fn"](), "abcdef123456")

    def test_install_desktop_setup_command_helpers_routes_each_group(self) -> None:
        bindings = self.bindings("cmd_desktop_install", lambda *args, **kwargs: 31)
        bindings["_desktop_setup_commands_cli"].cmd_desktop_doctor = lambda *args, **kwargs: 37

        self.mod.install_desktop_setup_command_helpers(
            bindings,
            ("cmd_desktop_install", "cmd_desktop_doctor"),
        )

        self.assertEqual(bindings["cmd_desktop_install"](object()), 31)
        self.assertEqual(bindings["cmd_desktop_doctor"](object()), 37)

    def test_install_desktop_setup_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_setup_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_setup_command_helpers(bindings, ("future_desktop_setup_command_helper",))

        self.assertEqual(bindings["future_desktop_setup_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
