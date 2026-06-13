#!/usr/bin/env python3
"""Tests for utility command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("utility_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class UtilityCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_compose_focused_command_groups(self):
        self.assertEqual(
            self.mod.UTILITY_COMMAND_EXPORTS,
            (
                *self.mod.CLEANUP_COMMAND_EXPORTS,
                *self.mod.UTILITY_QUEUE_COMMAND_EXPORTS,
                *self.mod.LOGS_COMMAND_EXPORTS,
                *self.mod.EVIDENCE_COMMAND_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.UTILITY_COMMAND_EXPORTS), len(set(self.mod.UTILITY_COMMAND_EXPORTS)))

    def _bindings(self, module_name: str, runner_name: str, runner):
        bindings = {
            module_name: types.SimpleNamespace(**{runner_name: runner}),
            "_queue_orchestrator": types.SimpleNamespace(select_job_for_logs=object()),
        }
        for name in [
            "local_ci_state_footprint",
            "state_footprint_lines",
            "cleanup_plan_lines",
            "load_queue",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "print_local_ci_cleanup_plan",
            "print_local_ci_state_footprint",
            "format_size_bytes",
            "describe_path_for_cleanup",
            "normalize_priority",
            "bump_queue_command_job",
            "bump_queue_command_result_line",
            "cancel_queue_command_job",
            "cancel_queue_command_result_line",
            "current_runner_info",
            "resolve_job_for_logs",
            "target_log_path",
            "job_logs_dir",
            "tail_lines",
            "missing_job_logs_line",
            "missing_log_files_line",
            "job_logs_header_line",
            "log_section_header_line",
            "empty_log_line",
            "current_branch",
            "evidence_scope_header_line",
            "print_evidence_summary",
            "evidence_empty_line",
        ]:
            bindings[name] = object()
        return bindings

    def test_cleanup_footprint_and_plan_bind_facade_dependencies(self):
        captured = {}

        def footprint_runner(**kwargs):
            captured["footprint"] = kwargs

        bindings = self._bindings("_cleanup_cli", "print_local_ci_state_footprint", footprint_runner)
        self.mod.print_local_ci_state_footprint(bindings, indent="  ")

        self.assertIs(captured["footprint"]["local_ci_state_footprint_fn"], bindings["local_ci_state_footprint"])
        self.assertIs(captured["footprint"]["state_footprint_lines_fn"], bindings["state_footprint_lines"])
        self.assertEqual(captured["footprint"]["indent"], "  ")

        def plan_runner(*args, **kwargs):
            captured["plan_args"] = args
            captured["plan_kwargs"] = kwargs

        bindings = self._bindings("_cleanup_cli", "print_local_ci_cleanup_plan", plan_runner)
        plan = {"remove": []}
        self.mod.print_local_ci_cleanup_plan(bindings, plan, dry_run=True)

        self.assertEqual(captured["plan_args"], (plan,))
        self.assertTrue(captured["plan_kwargs"]["dry_run"])
        self.assertIs(captured["plan_kwargs"]["cleanup_plan_lines_fn"], bindings["cleanup_plan_lines"])

    def test_cmd_cleanup_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = self._bindings("_cleanup_cli", "cmd_cleanup", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_cleanup(bindings, args_obj), 9)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_queue_fn"], bindings["load_queue"])
        self.assertIs(captured["kwargs"]["collect_cleanup_plan_fn"], bindings["collect_local_ci_cleanup_plan"])
        self.assertIs(captured["kwargs"]["apply_cleanup_plan_fn"], bindings["apply_local_ci_cleanup_plan"])
        self.assertIs(captured["kwargs"]["print_cleanup_plan_fn"], bindings["print_local_ci_cleanup_plan"])
        self.assertIs(captured["kwargs"]["print_state_footprint_fn"], bindings["print_local_ci_state_footprint"])
        self.assertIs(captured["kwargs"]["format_size_fn"], bindings["format_size_bytes"])
        self.assertIs(captured["kwargs"]["describe_path_fn"], bindings["describe_path_for_cleanup"])

    def test_queue_commands_bind_facade_dependencies(self):
        cases = [
            (
                "_queue_commands_cli",
                "cmd_bump",
                self.mod.cmd_bump,
                ["normalize_priority", "bump_queue_command_job", "bump_queue_command_result_line"],
            ),
            (
                "_queue_commands_cli",
                "cmd_cancel",
                self.mod.cmd_cancel,
                ["cancel_queue_command_job", "cancel_queue_command_result_line"],
            ),
        ]
        for module_name, runner_name, wrapper, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 4

                bindings = self._bindings(module_name, runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 4)
                self.assertEqual(captured["args"], (args_obj,))
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_logs_bind_facade_dependencies(self):
        captured = {}

        def resolve_runner(*args, **kwargs):
            captured["resolve_args"] = args
            captured["resolve_kwargs"] = kwargs
            return {"id": "job"}

        bindings = self._bindings("_logs_cli", "resolve_job_for_logs", resolve_runner)
        self.assertEqual(self.mod.resolve_job_for_logs(bindings, "job"), {"id": "job"})
        self.assertEqual(captured["resolve_args"], ("job",))
        self.assertIs(captured["resolve_kwargs"]["load_queue_fn"], bindings["load_queue"])
        self.assertIs(captured["resolve_kwargs"]["current_runner_info_fn"], bindings["current_runner_info"])
        self.assertIs(captured["resolve_kwargs"]["select_job_for_logs_fn"], bindings["_queue_orchestrator"].select_job_for_logs)

        def cmd_runner(*args, **kwargs):
            captured["cmd_args"] = args
            captured["cmd_kwargs"] = kwargs
            return 2

        bindings = self._bindings("_logs_cli", "cmd_logs", cmd_runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_logs(bindings, args_obj), 2)
        self.assertEqual(captured["cmd_args"], (args_obj,))
        for name in [
            "resolve_job_for_logs",
            "target_log_path",
            "job_logs_dir",
            "tail_lines",
            "missing_job_logs_line",
            "missing_log_files_line",
            "job_logs_header_line",
            "log_section_header_line",
            "empty_log_line",
        ]:
            self.assertIs(captured["cmd_kwargs"][f"{name}_fn"], bindings[name])

    def test_evidence_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 0

        bindings = self._bindings("_evidence_cli", "cmd_evidence", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_evidence(bindings, args_obj), 0)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "current_branch",
            "evidence_scope_header_line",
            "print_evidence_summary",
            "evidence_empty_line",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_install_utility_command_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 8

        bindings = self._bindings("_queue_commands_cli", "cmd_bump", runner)

        self.mod.install_utility_command_helpers(bindings, names=("cmd_bump",))

        args_obj = object()
        self.assertEqual(bindings["cmd_bump"](args_obj), 8)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["normalize_priority_fn"], bindings["normalize_priority"])
        self.assertEqual(bindings["cmd_bump"].__name__, "cmd_bump")

    def test_install_utility_command_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_utility_command_helper = lambda _bindings: "future"

        self.mod.install_utility_command_helpers(bindings, names=("future_utility_command_helper",))

        self.assertEqual(bindings["future_utility_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
