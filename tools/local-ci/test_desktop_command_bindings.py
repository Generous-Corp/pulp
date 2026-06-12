#!/usr/bin/env python3
"""Tests for desktop command facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_command_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("desktop_command_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, module_name: str, runner_name: str, runner):
        desktop_cli = types.SimpleNamespace(
            desktop_status_lines=object(),
            desktop_config_show_lines=object(),
            desktop_config_update_lines=object(),
            desktop_recent_lines=object(),
            desktop_proof_empty_line=object(),
            desktop_proof_lines=object(),
            desktop_publish_lines=object(),
            desktop_cleanup_empty_line=object(),
            desktop_cleanup_lines=object(),
            desktop_action_success_lines=object(),
        )
        bindings = {
            module_name: types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": desktop_cli,
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
            "desktop_doctor_checks",
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
            "config_path",
            "normalize_publish_mode",
            "parse_config_bool",
            "normalize_desktop_config",
            "stage_desktop_publish_report",
            "prune_desktop_run_manifests",
            "write_desktop_run_rollups",
            "make_desktop_source_request",
            "run_macos_local_smoke",
            "run_linux_xvfb_remote_action",
            "run_windows_session_agent_action",
        ]:
            bindings[name] = object()
        return bindings

    def test_cmd_desktop_install_binds_setup_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 7

        bindings = self._bindings("_desktop_setup_commands_cli", "cmd_desktop_install", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_install(bindings, args_obj), 7)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "load_config",
            "resolve_desktop_target",
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
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
        self.assertIs(captured["kwargs"]["check_writable_dir_fn"], bindings["_check_writable_dir"])
        self.assertIs(captured["kwargs"]["subprocess_run_fn"], bindings["subprocess"].run)
        self.assertIs(captured["kwargs"]["root_path"], bindings["ROOT"])
        self.assertEqual(captured["kwargs"]["new_install_job_id_fn"](), "abcdef123456")

    def test_cmd_desktop_doctor_and_status_bind_dependencies(self):
        cases = [
            (
                "_desktop_setup_commands_cli",
                "cmd_desktop_doctor",
                self.mod.cmd_desktop_doctor,
                ["load_config", "resolve_desktop_target", "desktop_doctor_checks"],
            ),
            (
                "_desktop_commands_cli",
                "cmd_desktop_status",
                self.mod.cmd_desktop_status,
                [
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
                ],
            ),
        ]
        for module_name, runner_name, wrapper, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 3

                bindings = self._bindings(module_name, runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 3)
                self.assertEqual(captured["args"], (args_obj,))
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
                if runner_name == "cmd_desktop_status":
                    self.assertIs(captured["kwargs"]["desktop_status_lines_fn"], bindings["_desktop_cli"].desktop_status_lines)

    def test_desktop_config_recent_proof_publish_cleanup_bind_dependencies(self):
        cases = [
            (
                "cmd_desktop_config_show",
                self.mod.cmd_desktop_config_show,
                ["load_config"],
                {"desktop_config_show_lines_fn": "desktop_config_show_lines"},
            ),
            (
                "cmd_desktop_config_set",
                self.mod.cmd_desktop_config_set,
                ["load_config", "save_config", "config_path", "normalize_publish_mode", "parse_config_bool", "normalize_desktop_config"],
                {"desktop_config_update_lines_fn": "desktop_config_update_lines"},
            ),
            (
                "cmd_desktop_recent",
                self.mod.cmd_desktop_recent,
                ["load_config", "desktop_run_manifests", "desktop_run_summary", "short_sha"],
                {"desktop_recent_lines_fn": "desktop_recent_lines"},
            ),
            (
                "cmd_desktop_proof",
                self.mod.cmd_desktop_proof,
                ["load_config", "desktop_proof_summaries", "short_sha"],
                {"desktop_proof_empty_line_fn": "desktop_proof_empty_line", "desktop_proof_lines_fn": "desktop_proof_lines"},
            ),
            (
                "cmd_desktop_publish",
                self.mod.cmd_desktop_publish,
                ["load_config", "desktop_run_manifests", "stage_desktop_publish_report"],
                {"desktop_publish_lines_fn": "desktop_publish_lines"},
            ),
            (
                "cmd_desktop_cleanup",
                self.mod.cmd_desktop_cleanup,
                ["load_config", "prune_desktop_run_manifests", "write_desktop_run_rollups"],
                {"desktop_cleanup_empty_line_fn": "desktop_cleanup_empty_line", "desktop_cleanup_lines_fn": "desktop_cleanup_lines"},
            ),
        ]
        for runner_name, wrapper, dependency_names, desktop_cli_bindings in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 4

                bindings = self._bindings("_desktop_commands_cli", runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 4)
                self.assertEqual(captured["args"], (args_obj,))
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
                for kwarg, attr in desktop_cli_bindings.items():
                    self.assertIs(captured["kwargs"][kwarg], getattr(bindings["_desktop_cli"], attr))

    def test_windows_selector_helper_and_action_commands_bind_dependencies(self):
        captured = {}

        def selector_runner(args):
            captured["selector_args"] = args
            return True

        bindings = self._bindings("_desktop_action_commands_cli", "windows_requires_pulp_app_selectors", selector_runner)
        args_obj = object()
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(bindings, args_obj))
        self.assertIs(captured["selector_args"], args_obj)

        for runner_name, wrapper in [
            ("cmd_desktop_smoke", self.mod.cmd_desktop_smoke),
            ("cmd_desktop_click", self.mod.cmd_desktop_click),
            ("cmd_desktop_inspect", self.mod.cmd_desktop_inspect),
        ]:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 5

                bindings = self._bindings("_desktop_action_commands_cli", runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 5)
                self.assertEqual(captured["args"], (args_obj,))
                for name in [
                    "load_config",
                    "resolve_desktop_target",
                    "make_desktop_source_request",
                    "run_macos_local_smoke",
                    "run_linux_xvfb_remote_action",
                    "run_windows_session_agent_action",
                ]:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
                self.assertIs(captured["kwargs"]["desktop_action_success_lines_fn"], bindings["_desktop_cli"].desktop_action_success_lines)
                self.assertEqual(captured["kwargs"]["sys_platform"], "darwin")

    def test_install_desktop_command_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 11

        bindings = self._bindings("_desktop_commands_cli", "cmd_desktop_recent", runner)

        self.mod.install_desktop_command_helpers(bindings, names=("cmd_desktop_recent",))

        args_obj = object()
        self.assertEqual(bindings["cmd_desktop_recent"](args_obj), 11)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_recent_lines_fn"], bindings["_desktop_cli"].desktop_recent_lines)
        self.assertEqual(bindings["cmd_desktop_recent"].__name__, "cmd_desktop_recent")


if __name__ == "__main__":
    unittest.main()
