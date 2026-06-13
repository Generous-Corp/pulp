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


if __name__ == "__main__":
    unittest.main()
