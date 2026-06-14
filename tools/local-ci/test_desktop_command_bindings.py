#!/usr/bin/env python3
"""Tests for desktop command facade composition."""

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SETUP_COMMAND_EXPORTS,
            *self.mod.DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
            *self.mod.DESKTOP_ACTION_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_command_helpers_routes_each_group(self):
        bindings = {
            "_desktop_setup_commands_cli": types.SimpleNamespace(cmd_desktop_install=lambda *args, **kwargs: 53),
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_status=lambda *args, **kwargs: 59),
            "_desktop_action_commands_cli": types.SimpleNamespace(cmd_desktop_smoke=lambda *args, **kwargs: 61),
            "_desktop_cli": types.SimpleNamespace(
                desktop_status_lines=object(),
                desktop_action_success_lines=object(),
            ),
            "ROOT": Path("/repo"),
            "subprocess": types.SimpleNamespace(run=object()),
            "sys": types.SimpleNamespace(platform="darwin"),
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
            "desktop_receipt_for",
            "desktop_capabilities_for",
            "desktop_optional_capabilities",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "normalize_desktop_optional_config",
            "desktop_publish_reports",
            "short_sha",
            "windows_repo_checkout_detail",
            "make_desktop_source_request",
            "run_macos_local_smoke",
            "run_linux_xvfb_remote_action",
            "run_windows_session_agent_action",
        ]:
            bindings[name] = object()

        self.mod.install_desktop_command_helpers(
            bindings,
            ("cmd_desktop_install", "cmd_desktop_status", "cmd_desktop_smoke"),
        )

        self.assertEqual(bindings["cmd_desktop_install"](object()), 53)
        self.assertEqual(bindings["cmd_desktop_status"](object()), 59)
        self.assertEqual(bindings["cmd_desktop_smoke"](object()), 61)
        self.assertNotIn("cmd_desktop_cleanup", bindings)


if __name__ == "__main__":
    unittest.main()
