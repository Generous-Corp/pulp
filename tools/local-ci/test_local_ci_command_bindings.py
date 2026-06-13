#!/usr/bin/env python3
"""Tests for top-level local-CI command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        commands_cli = types.SimpleNamespace(**{runner_name: runner})
        bindings = {
            "_local_ci_commands_cli": commands_cli,
            "ROOT": Path("/repo"),
            "subprocess": types.SimpleNamespace(run=object()),
        }
        for name in [
            "load_config",
            "current_branch",
            "resolve_git_ref_sha",
            "current_sha",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
            "resolve_submission_options",
            "print_submission_metadata",
            "enqueue_job",
            "enqueue_command_result_line",
            "drain_pending_jobs",
            "current_runner_info",
            "drain_runner_active_line",
            "notify",
            "gh_workflow_dispatch",
            "wait_for_job",
            "load_job",
            "print_result",
            "gh_available",
            "gh_pr_create",
            "summarize_job",
            "gh_pr_comment",
            "format_ci_comment",
            "gh_pr_merge",
            "gh_pr_head",
            "short_sha",
            "gh_pr_list_open",
            "open_pr_list_lines",
            "load_queue",
            "queue_status_groups",
            "state_dir",
            "config_path",
            "status_runner_line",
            "status_submission_lines",
            "status_active_targets",
            "summarize_active_targets",
            "status_target_detail_lines",
            "recent_completed_jobs_for_status",
            "load_result",
            "recent_completed_status_line",
            "recent_completed_missing_result_line",
            "print_evidence_summary",
            "list_cloud_records",
            "load_optional_config",
            "github_actions_settings_for_display",
            "resolve_github_actions_settings",
            "resolve_default_provider_for_workflow",
            "print_billing_period_summary",
            "estimate_billing_period_totals",
            "cloud_record_summary",
            "print_local_ci_state_footprint",
            "utmctl_vm_status",
            "ssh_reachable",
        ]:
            bindings[name] = object()
        return bindings

    def test_resolve_submission_options_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ({}, "branch", "sha", ["mac"], "normal", "full", {})

        bindings = self._bindings("resolve_submission_options", runner)
        args_obj = object()
        result = self.mod.resolve_submission_options(bindings, args_obj, "run")

        self.assertEqual(result[1], "branch")
        self.assertEqual(captured["args"], (args_obj, "run"))
        for name in [
            "load_config",
            "current_branch",
            "resolve_git_ref_sha",
            "current_sha",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_enqueue_drain_run_and_list_bind_facade_dependencies(self):
        cases = [
            (
                "cmd_enqueue",
                self.mod.cmd_enqueue,
                [
                    "resolve_submission_options",
                    "print_submission_metadata",
                    "enqueue_job",
                    "enqueue_command_result_line",
                ],
            ),
            (
                "cmd_drain",
                self.mod.cmd_drain,
                ["load_config", "drain_pending_jobs", "current_runner_info", "drain_runner_active_line", "notify"],
            ),
            (
                "cmd_run",
                self.mod.cmd_run,
                [
                    "resolve_submission_options",
                    "print_submission_metadata",
                    "gh_workflow_dispatch",
                    "enqueue_job",
                    "enqueue_command_result_line",
                    "wait_for_job",
                    "load_job",
                    "print_result",
                    "notify",
                ],
            ),
            ("cmd_list", self.mod.cmd_list, ["gh_available", "gh_pr_list_open", "open_pr_list_lines"]),
        ]

        for runner_name, wrapper, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 7

                bindings = self._bindings(runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 7)
                self.assertEqual(captured["args"], (args_obj,))
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_cmd_ship_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self._bindings("cmd_ship", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_ship(bindings, args_obj), 5)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["resolve_submission_options_fn"], bindings["resolve_submission_options"])
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["run_fn"], bindings["subprocess"].run)
        self.assertIs(captured["kwargs"]["gh_pr_merge_fn"], bindings["gh_pr_merge"])

    def test_cmd_check_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 3

        bindings = self._bindings("cmd_check", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_check(bindings, args_obj), 3)

        for name in [
            "gh_available",
            "gh_pr_head",
            "short_sha",
            "load_config",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
            "print_submission_metadata",
            "enqueue_job",
            "summarize_job",
            "wait_for_job",
            "gh_pr_comment",
            "format_ci_comment",
            "notify",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_cmd_status_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 0

        bindings = self._bindings("cmd_status", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_status(bindings, args_obj), 0)

        status_dependency_names = [
            "load_config",
            "load_queue",
            "queue_status_groups",
            "current_runner_info",
            "state_dir",
            "config_path",
            "status_runner_line",
            "summarize_job",
            "status_submission_lines",
            "status_active_targets",
            "summarize_active_targets",
            "status_target_detail_lines",
            "recent_completed_jobs_for_status",
            "load_result",
            "recent_completed_status_line",
            "recent_completed_missing_result_line",
            "current_branch",
            "print_evidence_summary",
            "list_cloud_records",
            "load_optional_config",
            "github_actions_settings_for_display",
            "resolve_github_actions_settings",
            "resolve_default_provider_for_workflow",
            "print_billing_period_summary",
            "estimate_billing_period_totals",
            "cloud_record_summary",
            "utmctl_vm_status",
            "ssh_reachable",
        ]
        for name in status_dependency_names:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
        self.assertIs(captured["kwargs"]["print_state_footprint_fn"], bindings["print_local_ci_state_footprint"])

    def test_install_local_ci_command_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 12

        bindings = self._bindings("cmd_list", runner)

        self.mod.install_local_ci_command_helpers(bindings, names=("cmd_list",))

        args_obj = object()
        self.assertEqual(bindings["cmd_list"](args_obj), 12)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertEqual(bindings["cmd_list"].__name__, "cmd_list")

    def test_install_local_ci_command_helpers_keeps_unknown_local_fallback(self):
        bindings = self._bindings("cmd_list", lambda *_args, **_kwargs: 0)
        self.mod.future_local_ci_command = lambda _bindings, args: ("future", args)

        self.mod.install_local_ci_command_helpers(bindings, names=("future_local_ci_command",))

        args_obj = object()
        self.assertEqual(bindings["future_local_ci_command"](args_obj), ("future", args_obj))


if __name__ == "__main__":
    unittest.main()
