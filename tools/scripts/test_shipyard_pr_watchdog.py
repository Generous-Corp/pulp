#!/usr/bin/env python3
from __future__ import annotations

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
