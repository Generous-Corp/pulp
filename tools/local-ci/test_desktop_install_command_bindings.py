#!/usr/bin/env python3
"""Tests for desktop install command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_install_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInstallCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_INSTALL_COMMAND_EXPORTS, ("cmd_desktop_install",))

    def test_install_binds_setup_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(cmd_desktop_install=runner),
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
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_install(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
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
        ]:
            suffix = "check_writable_dir" if name == "_check_writable_dir" else name
            self.assertIs(captured["kwargs"][f"{suffix}_fn"], bindings[name])
        self.assertIs(captured["kwargs"]["subprocess_run_fn"], bindings["subprocess"].run)
        self.assertEqual(captured["kwargs"]["root_path"], Path("/repo"))
        self.assertEqual(captured["kwargs"]["new_install_job_id_fn"](), "abcdef123456")

    def test_install_desktop_install_command_helpers_wires_named_exports(self) -> None:
        def runner(*args, **kwargs):
            return 23

        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(cmd_desktop_install=runner),
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
        ]:
            bindings[name] = object()

        self.mod.install_desktop_install_command_helpers(bindings, ("cmd_desktop_install",))

        self.assertEqual(bindings["cmd_desktop_install"](object()), 23)

    def test_install_desktop_install_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_install_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_install_command_helpers(bindings, ("future_desktop_install_command_helper",))

        self.assertEqual(bindings["future_desktop_install_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
