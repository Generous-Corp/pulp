#!/usr/bin/env python3
"""Regression for the Darwin G-9 descendant-survival observation."""
import signal
import unittest
from pathlib import Path
from subprocess import TimeoutExpired
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

from g9_hostile_fixture import CRASH_REPORTING_SIGNALS, is_live_process_state, run_case


class ProcessStateTest(unittest.TestCase):
    def test_sleeping_descendant_is_live(self) -> None:
        self.assertTrue(is_live_process_state("S"))

    def test_absent_descendant_is_not_live(self) -> None:
        self.assertFalse(is_live_process_state(None))

    def test_zombie_descendant_is_not_live(self) -> None:
        for state in ("Z", "Z+", "SZ", "z"):
            with self.subTest(state=state):
                self.assertFalse(is_live_process_state(state))

    def test_receipt_rejects_zombie_after_parent_exit(self) -> None:
        parent = Mock(pid=41, returncode=-15)
        parent.communicate.side_effect = [TimeoutExpired("fixture", 0.5), ("42\n", "")]
        with TemporaryDirectory() as directory, \
                patch("g9_hostile_fixture.subprocess.Popen", return_value=parent), \
                patch("g9_hostile_fixture.process_state", side_effect=[None, "Z+"]), \
                patch("g9_hostile_fixture.cleanup_descendant", return_value={"gone_after_cleanup": True}):
            record = run_case("descendant", 0.5, Path(directory))
        self.assertTrue(record["parent_gone"])
        self.assertEqual(record["descendant_state_after_parent"], "Z+")
        self.assertFalse(record["descendant_alive_after_parent"])


class CrashCaseTest(unittest.TestCase):
    def test_crash_case_dies_by_signal_without_crash_report(self) -> None:
        with TemporaryDirectory() as directory:
            record = run_case("crash", 2.0, Path(directory))
        self.assertTrue(record["detected"])
        self.assertFalse(record["timed_out"])
        self.assertLess(record["returncode"], 0, "crash case must die by a signal")
        died_by = signal.Signals(-record["returncode"])
        self.assertNotIn(died_by, CRASH_REPORTING_SIGNALS,
                         f"{died_by.name} makes ReportCrash write a bash .ips report")


if __name__ == "__main__":
    unittest.main()
