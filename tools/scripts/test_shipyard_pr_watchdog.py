#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import pathlib
import os
import subprocess
import sys
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).with_name("shipyard_pr_watchdog.py")
SPEC = importlib.util.spec_from_file_location("shipyard_pr_watchdog", SCRIPT)
assert SPEC and SPEC.loader
WATCHDOG = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = WATCHDOG
SPEC.loader.exec_module(WATCHDOG)


class WatchdogUnitTests(unittest.TestCase):
    def test_descendants_are_transitive(self) -> None:
        self.assertEqual(WATCHDOG.descendants(1, {1: [2], 2: [3], 3: [4]}), {2, 3, 4})

    def test_failed_process_snapshot_fails_open(self) -> None:
        with mock.patch.object(WATCHDOG.subprocess, "run") as run:
            run.return_value.returncode = 1
            self.assertIsNone(WATCHDOG.process_snapshot())

    def test_cpu_seconds_reads_both_ps_time_formats(self) -> None:
        cases = {
            "0:00.12": 0.12,         # macOS mm:ss.ff
            "1:02.50": 62.5,
            "00:00:07": 7.0,          # Linux hh:mm:ss
            "01:00:00": 3600.0,
            "2-03:00:01": 2 * 86400 + 3 * 3600 + 1,
        }
        for value, expected in cases.items():
            with self.subTest(value=value):
                self.assertAlmostEqual(WATCHDOG.cpu_seconds(value), expected)

    def test_cpu_rate_comes_from_the_change_between_polls(self) -> None:
        # Busy at startup, idle since: the cumulative total is large, but it
        # has not moved, so the rate is zero. A lifetime average would still
        # read as busy here, which is the Linux `%cpu` behaviour.
        self.assertEqual(WATCHDOG.cpu_percent((10.0, 4.0), 12.0, 4.0), 0.0)
        self.assertAlmostEqual(WATCHDOG.cpu_percent((10.0, 4.0), 12.0, 5.0), 50.0)
        self.assertIsNone(WATCHDOG.cpu_percent(None, 12.0, 4.0))
        self.assertIsNone(WATCHDOG.cpu_percent((10.0, 4.0), 12.0, None))

    def test_snapshot_parses_cumulative_cpu(self) -> None:
        with mock.patch.object(WATCHDOG.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "  10     1 0:01.50\n  11    10 00:00:02\n"
            children, cpu = WATCHDOG.process_snapshot()
        self.assertEqual(children, {1: [10], 10: [11]})
        self.assertEqual(cpu, {10: 1.5, 11: 2.0})

    def test_first_cpu_reading_never_stops_a_process(self) -> None:
        # With zero stall and confirm windows, a process whose CPU total never
        # moves is stopped as soon as it reads idle. The first reading has no
        # rate yet, so it must count as busy: the stop comes on the second
        # snapshot, never the first.
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding="utf-8", start_new_session=True)
        self.addCleanup(lambda: (child.kill(), child.wait(), child.stdout.close()))
        snapshots: list[int] = []

        def snapshot():
            snapshots.append(1)
            return {}, {child.pid: 1.0}

        args = argparse.Namespace(
            poll_seconds=0.01, stall_seconds=0.0, confirm_seconds=0.0,
            stop_grace_seconds=0.1, max_idle_cpu=0.5)
        with mock.patch.object(WATCHDOG.subprocess, "Popen", return_value=child), \
                mock.patch.object(WATCHDOG, "process_snapshot", side_effect=snapshot), \
                mock.patch.object(WATCHDOG, "capture_diagnostic"), \
                mock.patch.object(WATCHDOG, "stop_process_group",
                                  side_effect=lambda proc, _grace: (proc.kill(), proc.wait())):
            _rc, stalled = WATCHDOG.run_once(["unused"], args)
        self.assertTrue(stalled)
        self.assertEqual(len(snapshots), 2)

    def test_success_exit_is_preserved(self) -> None:
        result = self.run_watchdog("print('ok')")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ok", result.stdout)

    def test_failure_exit_is_preserved(self) -> None:
        result = self.run_watchdog("raise SystemExit(7)")
        self.assertEqual(result.returncode, 7)

    def test_quiet_live_child_is_not_killed(self) -> None:
        result = self.run_watchdog(
            "import subprocess,sys; subprocess.run([sys.executable,'-c','import time; time.sleep(.35)'])",
            stall=.1,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_periodic_output_resets_stall_clock(self) -> None:
        result = self.run_watchdog(
            "import time; [(print(i, flush=True), time.sleep(.08)) for i in range(5)]",
            stall=.12,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_orphaned_pipe_state_restarts_once_then_fails(self) -> None:
        result = self.run_watchdog("import time; time.sleep(5)", stall=.1, confirm=.1)
        self.assertEqual(result.returncode, 124)
        self.assertEqual(result.stderr.count("restarting once"), 1)
        self.assertIn("repeated stall", result.stderr)

    def run_watchdog(
        self, code: str, *, stall: float = .2, confirm: float = .1
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--stall-seconds", str(stall),
                "--confirm-seconds", str(confirm),
                "--poll-seconds", ".03",
                "--stop-grace-seconds", ".1",
                "--",
                sys.executable,
                "-c",
                code,
            ],
            capture_output=True,
            text=True,
            timeout=5,
            env={**os.environ, "PULP_SHIPYARD_WATCHDOG_DIAGNOSTICS": "0"},
        )


if __name__ == "__main__":
    unittest.main()
