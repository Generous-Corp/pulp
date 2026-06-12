#!/usr/bin/env python3
"""Tests for local-CI status command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_status_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiStatusCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_cmd_status_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 0

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_status=runner)}
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
            "print_local_ci_state_footprint",
            "utmctl_vm_status",
            "ssh_reachable",
        ]
        for name in status_dependency_names:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_status(bindings, args_obj), 0)
        self.assertEqual(captured["args"], (args_obj,))
        for name in status_dependency_names:
            expected_kwarg = "print_state_footprint_fn" if name == "print_local_ci_state_footprint" else f"{name}_fn"
            self.assertIs(captured["kwargs"][expected_kwarg], bindings[name])


if __name__ == "__main__":
    unittest.main()
