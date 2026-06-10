#!/usr/bin/env python3
"""Tests for local CI command execution helpers."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution.py")
MODULE_DIR = MODULE_PATH.parent


def load_module():
    sys.path.insert(0, str(MODULE_DIR))
    try:
        spec = importlib.util.spec_from_file_location("pulp_local_ci_execution", MODULE_PATH)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class ExecutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_parse_progress_marker_detects_progress_contract(self) -> None:
        self.assertEqual(self.mod.parse_progress_marker("__PULP_PHASE__:build\n"), {"phase": "build"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_WAIT__:host-lock\n"), {"wait_reason": "host-lock"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATION__:smoke\n"), {"validation_mode": "smoke"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_TEST_POLICY__:skip\n"), {"test_policy": "skip"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_PREPARED__:reused\n"), {"prepared_state": "reused"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATOR_PID__:4321\n"), {"validator_pid": 4321})
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_STARTED__:2026-04-02T04:00:00+00:00\n"),
            {"validator_started_at": "2026-04-02T04:00:00+00:00"},
        )
        self.assertEqual(self.mod.parse_progress_marker("normal output\n"), {})

    def test_validation_helper_policy_matches_contract(self) -> None:
        job = {"sha": "abcdef1234567890", "targets": ["mac"]}
        self.assertEqual(
            self.mod.remote_commit_error("ubuntu", "ubuntu.local", job),
            (
                "ubuntu cannot validate abcdef123456 on ubuntu.local: "
                "commit is not available on origin. Push the branch first or use --targets mac."
            ),
        )
        self.assertEqual(
            self.mod.prepared_state_root("mac", " SMOKE "),
            self.mod.state_dir() / "prepared" / "mac" / "smoke",
        )
        self.assertTrue(self.mod.should_reuse_prepared_state({"targets": ["mac"]}))
        self.assertFalse(self.mod.should_reuse_prepared_state({"targets": ["mac", "ubuntu"]}))

    def test_validation_result_from_run_reports_timeout(self) -> None:
        result = self.mod.validation_result_from_run(
            "mac",
            {"timed_out": True, "duration_secs": 3600.0},
            log_path=Path("mac.log"),
            validation="full",
            transport_mode="local",
        )

        self.assertEqual(
            result,
            {
                "target": "mac",
                "status": "timeout",
                "exit_code": -1,
                "duration_secs": 3600.0,
                "stdout_tail": "",
                "stderr_tail": "Validation timed out after 3600s",
                "log_file": "mac.log",
                "transport_mode": "local",
            },
        )

    def test_validation_result_from_run_splits_pass_and_fail_tails(self) -> None:
        passed = self.mod.validation_result_from_run(
            "ubuntu",
            {"timed_out": False, "returncode": 0, "output": "ok\n", "duration_secs": 1.2},
            log_path=Path("ubuntu.log"),
            validation="full",
            transport_mode="bundle",
        )
        failed = self.mod.validation_result_from_run(
            "ubuntu",
            {"timed_out": False, "returncode": 7, "output": "boom\n", "duration_secs": 1.3},
            log_path=Path("ubuntu.log"),
            validation="full",
            transport_mode="bundle",
        )

        self.assertEqual(passed["status"], "pass")
        self.assertEqual(passed["stdout_tail"], "ok\n")
        self.assertEqual(passed["stderr_tail"], "")
        self.assertEqual(failed["status"], "fail")
        self.assertEqual(failed["stdout_tail"], "")
        self.assertEqual(failed["stderr_tail"], "boom\n")

    def test_validation_result_from_run_enforces_smoke_contract(self) -> None:
        result = self.mod.validation_result_from_run(
            "windows",
            {"timed_out": False, "returncode": 0, "output": "ok\n", "duration_secs": 1.2},
            log_path=Path("windows.log"),
            validation="smoke",
            transport_mode="bundle",
        )

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])
        self.assertEqual(result["stdout_tail"], "")

    def test_run_logged_command_starts_reader_before_writing_input(self) -> None:
        read_started = threading.Event()
        read_finished = threading.Event()
        writes = []

        class FakeStdout:
            def __iter__(self):
                read_started.set()
                yield "ready\n"
                read_finished.set()

            def close(self):
                read_finished.set()

        class FakeStdin:
            def write(self, text):
                if not read_started.wait(timeout=1):
                    raise TimeoutError("reader did not start before stdin write")
                writes.append(text)

            def close(self):
                writes.append("<closed>")

        class FakeProc:
            def __init__(self):
                self.stdin = FakeStdin()
                self.stdout = FakeStdout()

            def poll(self):
                return 0 if read_finished.is_set() else None

            def wait(self, timeout=None):
                self.poll()
                read_finished.wait(timeout=timeout)
                return 0

            def kill(self):
                read_finished.set()

        original_popen = self.mod.subprocess.Popen
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        try:
            result = self.mod.run_logged_command(["ssh", "win2"], input_text="payload", timeout=5)
        finally:
            self.mod.subprocess.Popen = original_popen

        self.assertFalse(result["timed_out"])
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["output"], "ready\n")
        self.assertEqual(writes, ["payload", "<closed>"])

    def test_run_logged_command_keeps_markers_logs_and_reports_progress(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "marker.log"
            seen = {}

            def report_progress(**fields):
                seen.update(fields)

            result = self.mod.run_logged_command(
                [
                    "python3",
                    "-c",
                    (
                        "print('__PULP_VALIDATION__:smoke');"
                        "print('__PULP_TEST_POLICY__:skip');"
                        "print('__PULP_PHASE__:build');"
                        "print('done')"
                    ),
                ],
                log_path=log_path,
                report_progress=report_progress,
            )

            self.assertEqual(result["returncode"], 0)
            self.assertIn("__PULP_VALIDATION__:smoke", result["output"])
            self.assertIn("__PULP_TEST_POLICY__:skip", result["output"])
            self.assertIn("__PULP_PHASE__:build", result["output"])
            self.assertIn("done", result["output"])
            logged = log_path.read_text()
            self.assertIn("__PULP_VALIDATION__:smoke", logged)
            self.assertIn("__PULP_TEST_POLICY__:skip", logged)
            self.assertEqual(seen["validation_mode"], "smoke")
            self.assertEqual(seen["test_policy"], "skip")
            self.assertEqual(seen["phase"], "build")

    def test_run_logged_command_emits_quiet_heartbeat_and_stuck_state(self) -> None:
        seen: list[dict] = []

        def report_progress(**fields):
            seen.append(dict(fields))

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                "import time; time.sleep(0.18); print('done')",
            ],
            report_progress=report_progress,
            heartbeat_interval_secs=0.05,
            stuck_idle_secs=0.1,
        )

        self.assertEqual(result["returncode"], 0)
        heartbeat_events = [item for item in seen if item.get("last_heartbeat_at")]
        self.assertTrue(heartbeat_events, msg=f"missing heartbeat events in {seen}")
        liveness_values = {item.get("liveness") for item in heartbeat_events}
        self.assertTrue(liveness_values <= {"quiet", "stuck"}, msg=f"unexpected heartbeat states in {heartbeat_events}")
        self.assertTrue("stuck" in liveness_values, msg=f"missing stuck heartbeat in {heartbeat_events}")
        self.assertTrue(any(item.get("last_output_at") for item in seen))

    def test_run_logged_command_replaces_invalid_utf8_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "nonutf8.log"

            result = self.mod.run_logged_command(
                [
                    "python3",
                    "-c",
                    (
                        "import sys; "
                        "sys.stdout.buffer.write(b'prefix\\xe5suffix\\\\n'); "
                        "sys.stdout.flush()"
                    ),
                ],
                log_path=log_path,
            )

            self.assertEqual(result["returncode"], 0)
            self.assertIn("prefix", result["output"])
            self.assertIn("suffix", result["output"])
            self.assertIn("\ufffd", result["output"])
            logged = log_path.read_text()
            self.assertIn("prefix", logged)
            self.assertIn("suffix", logged)


if __name__ == "__main__":
    unittest.main()
