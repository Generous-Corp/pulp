#!/usr/bin/env python3
"""No-network tests for validation logged command execution."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("validation_logged_command.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class ValidationLoggedCommandTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_logged_command_keeps_markers_logs_and_reports_progress(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = pathlib.Path(tmp) / "marker.log"
            seen = {}

            def report_progress(**fields):
                seen.update(fields)

            result = self.mod.run_logged_command(
                [
                    sys.executable,
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
                heartbeat_interval_secs=15.0,
                stuck_idle_secs=90.0,
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
                sys.executable,
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


if __name__ == "__main__":
    unittest.main()
