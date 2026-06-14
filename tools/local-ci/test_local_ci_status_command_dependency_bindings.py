#!/usr/bin/env python3
"""Tests for local-CI status command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest


STATUS_DEPENDENCY_NAMES = [
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


def load_module():
    return load_local_ci_module("local_ci_status_command_dependency_bindings.py")


class LocalCiStatusCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_status_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {name: object() for name in STATUS_DEPENDENCY_NAMES}

        deps = self.mod.local_ci_status_command_dependencies(bindings)

        for name in STATUS_DEPENDENCY_NAMES:
            expected_kwarg = "print_state_footprint_fn" if name == "print_local_ci_state_footprint" else f"{name}_fn"
            self.assertIs(deps[expected_kwarg], bindings[name])


if __name__ == "__main__":
    unittest.main()
