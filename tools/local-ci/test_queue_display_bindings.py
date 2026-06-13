#!/usr/bin/env python3
"""Tests for queue display facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_display_exports_match_focused_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_COMMAND_DISPLAY_EXPORTS,
            *self.mod.QUEUE_STATUS_DISPLAY_EXPORTS,
            *self.mod.QUEUE_RESULT_DISPLAY_EXPORTS,
            *self.mod.QUEUE_LOG_DISPLAY_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_queue_display_bindings_delegate_to_orchestrator(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        orchestrator = types.SimpleNamespace(
            summarize_job=record("summarize_job", "summary"),
            bump_queue_command_result_line=record("bump_queue_command_result_line", (0, "bumped")),
            cancel_queue_command_result_line=record("cancel_queue_command_result_line", (0, "canceled")),
            enqueue_command_result_line=record("enqueue_command_result_line", "enqueued"),
            drain_runner_active_line=record("drain_runner_active_line", "active"),
            summarize_active_targets=record("summarize_active_targets", "targets"),
            status_active_targets=record("status_active_targets", {"mac": {}}),
            status_target_states=record("status_target_states", [("mac", {})]),
            status_submission_lines=record("status_submission_lines", ["submission"]),
            target_state_detail_parts=record("target_state_detail_parts", ["detail"]),
            status_target_detail_lines=record("status_target_detail_lines", ["target detail"]),
            status_runner_line=record("status_runner_line", "runner"),
            recent_completed_status_line=record("recent_completed_status_line", "recent"),
            recent_completed_missing_result_line=record("recent_completed_missing_result_line", "missing result"),
            result_validation_line=record("result_validation_line", "validation"),
            result_execution_line=record("result_execution_line", "execution"),
            target_result_line=record("target_result_line", "target"),
            result_target_lines=record("result_target_lines", ["target"]),
            result_overall_line=record("result_overall_line", "overall"),
            missing_job_logs_line=record("missing_job_logs_line", "missing logs"),
            missing_log_files_line=record("missing_log_files_line", "missing files"),
            job_logs_header_line=record("job_logs_header_line", "header"),
            log_section_header_line=record("log_section_header_line", "section"),
            empty_log_line=record("empty_log_line", "empty"),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.summarize_job(bindings, {"id": "job"}), "summary")
        self.assertEqual(self.mod.bump_queue_command_result_line(bindings, {"status": "bumped"}, "job"), (0, "bumped"))
        self.assertEqual(self.mod.cancel_queue_command_result_line(bindings, {"status": "canceled"}, "job"), (0, "canceled"))
        self.assertEqual(self.mod.enqueue_command_result_line(bindings, {"id": "job"}, created=True), "enqueued")
        self.assertEqual(self.mod.drain_runner_active_line(bindings, {"pid": 1}), "active")
        self.assertEqual(self.mod.summarize_active_targets(bindings, {"mac": {}}, ["mac"]), "targets")
        self.assertEqual(self.mod.status_active_targets(bindings, {"id": "job"}, {"mac": {}}), {"mac": {}})
        self.assertEqual(self.mod.status_target_states(bindings, {"id": "job"}, {"mac": {}}), [("mac", {})])
        self.assertEqual(self.mod.status_submission_lines(bindings, {"id": "job"}), ["submission"])
        self.assertEqual(self.mod.target_state_detail_parts(bindings, {"status": "pass"}), ["detail"])
        self.assertEqual(self.mod.status_target_detail_lines(bindings, {"id": "job"}, {"mac": {}}), ["target detail"])
        self.assertEqual(self.mod.status_runner_line(bindings, {"pid": 1}), "runner")
        self.assertEqual(self.mod.recent_completed_status_line(bindings, {"id": "job"}, {"overall": "pass"}), "recent")
        self.assertEqual(self.mod.recent_completed_missing_result_line(bindings, {"id": "job"}), "missing result")
        self.assertEqual(self.mod.result_validation_line(bindings, {"validation": "smoke"}), "validation")
        self.assertEqual(self.mod.result_execution_line(bindings, {"overall": "pass"}), "execution")
        self.assertEqual(self.mod.target_result_line(bindings, {"target": "mac"}), "target")
        self.assertEqual(self.mod.result_target_lines(bindings, {"targets": []}), ["target"])
        self.assertEqual(self.mod.result_overall_line(bindings, {"overall": "pass"}), "overall")
        self.assertEqual(self.mod.missing_job_logs_line(bindings), "missing logs")
        self.assertEqual(self.mod.missing_log_files_line(bindings, {"id": "job"}), "missing files")
        self.assertEqual(self.mod.job_logs_header_line(bindings, {"id": "job"}), "header")
        self.assertEqual(self.mod.log_section_header_line(bindings, "mac"), "section")
        self.assertEqual(self.mod.empty_log_line(bindings), "empty")

        self.assertEqual(calls[0], ("summarize_job", ({"id": "job"},), {}))
        self.assertEqual(calls[3], ("enqueue_command_result_line", ({"id": "job"},), {"created": True}))
        self.assertEqual(calls[5], ("summarize_active_targets", ({"mac": {}}, ["mac"]), {}))

    def test_install_queue_display_helpers_wires_named_exports(self):
        orchestrator = types.SimpleNamespace(
            summarize_job=lambda job: f"summary:{job['id']}",
            empty_log_line=lambda: "empty",
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.mod.install_queue_display_helpers(bindings, ("summarize_job", "empty_log_line"))

        self.assertEqual(bindings["summarize_job"]({"id": "job1"}), "summary:job1")
        self.assertEqual(bindings["empty_log_line"](), "empty")

    def test_install_queue_display_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_queue_display_helper = lambda _bindings: "future"

        self.mod.install_queue_display_helpers(bindings, ("future_queue_display_helper",))

        self.assertEqual(bindings["future_queue_display_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
