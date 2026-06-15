#!/usr/bin/env python3
"""Tests for desktop install command dependency assembly."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_install_command_dependency_bindings.py")


class DesktopInstallCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_install_dependencies_preserve_setup_seams(self) -> None:
        bindings = {
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

        deps = self.mod.desktop_install_command_dependencies(bindings)

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
            self.assertIs(deps[f"{suffix}_fn"], bindings[name])
        self.assertIs(deps["subprocess_run_fn"], bindings["subprocess"].run)
        self.assertEqual(deps["root_path"], Path("/repo"))
        self.assertEqual(deps["new_install_job_id_fn"](), "abcdef123456")


if __name__ == "__main__":
    unittest.main()
