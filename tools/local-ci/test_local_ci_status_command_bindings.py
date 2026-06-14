#!/usr/bin/env python3
"""Tests for local-CI status command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("local_ci_status_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiStatusCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_cmd_status_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 0

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_status=runner)}
        deps = {
            "load_config_fn": object(),
            "load_queue_fn": object(),
            "queue_status_groups_fn": object(),
            "current_runner_info_fn": object(),
            "state_dir_fn": object(),
            "config_path_fn": object(),
            "status_runner_line_fn": object(),
            "summarize_job_fn": object(),
            "status_submission_lines_fn": object(),
            "status_active_targets_fn": object(),
            "summarize_active_targets_fn": object(),
            "status_target_detail_lines_fn": object(),
            "recent_completed_jobs_for_status_fn": object(),
            "load_result_fn": object(),
            "recent_completed_status_line_fn": object(),
            "recent_completed_missing_result_line_fn": object(),
            "current_branch_fn": object(),
            "print_evidence_summary_fn": object(),
            "list_cloud_records_fn": object(),
            "load_optional_config_fn": object(),
            "github_actions_settings_for_display_fn": object(),
            "resolve_github_actions_settings_fn": object(),
            "resolve_default_provider_for_workflow_fn": object(),
            "print_billing_period_summary_fn": object(),
            "estimate_billing_period_totals_fn": object(),
            "cloud_record_summary_fn": object(),
            "print_state_footprint_fn": object(),
            "utmctl_vm_status_fn": object(),
            "ssh_reachable_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "local_ci_status_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_status(bindings, args_obj), 0)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)


if __name__ == "__main__":
    unittest.main()
