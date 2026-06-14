#!/usr/bin/env python3
"""Tests for desktop install command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_install_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInstallCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_INSTALL_COMMAND_EXPORTS, ("cmd_desktop_install",))

    def test_install_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(cmd_desktop_install=runner),
        }
        deps = {
            "load_config_fn": object(),
            "resolve_desktop_target_fn": object(),
            "check_writable_dir_fn": object(),
            "desktop_target_contract_fn": object(),
            "ensure_host_reachable_fn": object(),
            "bootstrap_windows_session_agent_fn": object(),
            "probe_windows_session_agent_fn": object(),
            "subprocess_run_fn": object(),
            "root_path": Path("/repo"),
            "new_install_job_id_fn": object(),
            "sync_job_bundle_to_ssh_host_fn": object(),
            "ensure_windows_remote_tooling_fn": object(),
            "windows_remote_tooling_ready_fn": object(),
            "ensure_windows_remote_repo_checkout_fn": object(),
            "git_origin_clone_url_fn": object(),
            "windows_repo_checkout_ready_fn": object(),
            "update_target_repo_path_fn": object(),
            "save_config_fn": object(),
            "now_iso_fn": object(),
            "desktop_target_receipt_path_fn": object(),
            "atomic_write_text_fn": object(),
            "windows_tooling_detail_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "desktop_install_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_install(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

if __name__ == "__main__":
    unittest.main()
