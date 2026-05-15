#!/usr/bin/env python3
"""Unit tests for tools/scripts/macos_reroute_watcher.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "macos_reroute_watcher.py"

spec = importlib.util.spec_from_file_location("macos_reroute_watcher", SCRIPT)
assert spec and spec.loader
watcher = importlib.util.module_from_spec(spec)
sys.modules["macos_reroute_watcher"] = watcher
spec.loader.exec_module(watcher)


class MacosRerouteWatcherTests(unittest.TestCase):
    def test_gh_returns_stripped_stdout(self) -> None:
        completed = subprocess.CompletedProcess(["gh"], 0, stdout="  ok\n")
        with mock.patch.object(watcher.subprocess, "run", return_value=completed) as run:
            self.assertEqual(watcher._gh(["repos/acme/project"]), "ok")
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0], ["gh", "api", "repos/acme/project"])
        self.assertEqual(run.call_args.kwargs["timeout"], 20)

    def test_local_is_busy_detects_runner_processes_and_failures(self) -> None:
        busy_output = (
            "/Users/runner/actions-runner/_work/pulp/pulp/_temp cmake --build build\n"
            "/usr/bin/other\n"
        )
        with mock.patch.object(
            watcher.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ps"], 0, stdout=busy_output),
        ):
            self.assertTrue(watcher.local_is_busy())

        worker_output = "/Users/runner/actions-runner/_work/pulp/pulp Runner.Worker spawnclient\n"
        with mock.patch.object(
            watcher.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ps"], 0, stdout=worker_output),
        ):
            self.assertTrue(watcher.local_is_busy())

        with mock.patch.object(
            watcher.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ps"], 0, stdout="/usr/bin/idle\n"),
        ):
            self.assertFalse(watcher.local_is_busy())

        with mock.patch.object(
            watcher.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(["ps"], timeout=10),
        ):
            self.assertIsNone(watcher.local_is_busy())

    def test_macos_job_target_detection(self) -> None:
        with mock.patch.object(watcher, "_gh", return_value="self-hosted,macOS"):
            self.assertFalse(watcher._macos_job_targets_cloud(10))

        for labels in ("macos-15,ARM64", "nscloud-macos,macOS", "namespace-profile-macos"):
            with self.subTest(labels=labels), mock.patch.object(watcher, "_gh", return_value=labels):
                self.assertTrue(watcher._macos_job_targets_cloud(10))

        with mock.patch.object(watcher, "_gh", return_value=""):
            self.assertFalse(watcher._macos_job_targets_cloud(10))

        with mock.patch.object(watcher, "_gh", side_effect=subprocess.CalledProcessError(1, ["gh"])):
            self.assertFalse(watcher._macos_job_targets_cloud(10))

    def test_list_queued_cloud_bat_runs_filters_invalid_and_local_rows(self) -> None:
        raw = "\n".join([
            json.dumps({"id": 101, "pr": 11}),
            "{not json",
            json.dumps({"id": None, "pr": 12}),
            json.dumps({"id": 102, "pr": None}),
            json.dumps({"id": 103, "pr": 13}),
        ])

        def fake_cloud(run_id: int) -> bool:
            return run_id == 101

        with mock.patch.object(watcher, "_gh", return_value=raw), \
             mock.patch.object(watcher, "_macos_job_targets_cloud", side_effect=fake_cloud):
            self.assertEqual(watcher.list_queued_cloud_bat_runs(), [(11, 101)])

        with mock.patch.object(watcher, "_gh", side_effect=subprocess.CalledProcessError(1, ["gh"])):
            self.assertEqual(watcher.list_queued_cloud_bat_runs(), [])

    def test_reroute_to_local_reports_success_and_failure(self) -> None:
        with mock.patch.object(
            watcher.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["pulp"], 0, stdout="done\n", stderr=""),
        ) as run:
            self.assertTrue(watcher.reroute_to_local(42))
        self.assertEqual(
            run.call_args.args[0],
            ["pulp", "macos", "retarget", "--pr", "42", "--to", "local"],
        )

        with mock.patch.object(
            watcher.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["pulp"], 2, stdout="", stderr="nope"),
        ):
            self.assertFalse(watcher.reroute_to_local(42))

        with mock.patch.object(
            watcher.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(["pulp"], timeout=60),
        ):
            self.assertFalse(watcher.reroute_to_local(42))

    def test_flap_guard_window_and_trim(self) -> None:
        guard = watcher.FlapGuard(window_seconds=10)
        with mock.patch.object(watcher.time, "time", return_value=100.0):
            self.assertTrue(guard.can_reroute(1))
            guard.record(1)
            self.assertFalse(guard.can_reroute(1))
        with mock.patch.object(watcher.time, "time", return_value=111.0):
            self.assertTrue(guard.can_reroute(1))

        guard = watcher.FlapGuard(window_seconds=10)
        guard._last[2] = 80.0
        guard._last[3] = 95.0
        with mock.patch.object(watcher.time, "time", return_value=111.0):
            guard.record(4)
        self.assertNotIn(2, guard._last)
        self.assertIn(3, guard._last)
        self.assertIn(4, guard._last)

    def test_tick_respects_busy_probe_candidates_flap_guard_and_one_reroute(self) -> None:
        guard = watcher.FlapGuard(window_seconds=300)

        with mock.patch.object(watcher, "local_is_busy", return_value=None), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs") as queued:
            watcher.tick(guard)
            queued.assert_not_called()

        with mock.patch.object(watcher, "local_is_busy", return_value=True), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs") as queued:
            watcher.tick(guard)
            queued.assert_not_called()

        with mock.patch.object(watcher, "local_is_busy", return_value=False), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs", return_value=[]), \
             mock.patch.object(watcher, "reroute_to_local") as reroute:
            watcher.tick(guard)
            reroute.assert_not_called()

        with mock.patch.object(watcher, "local_is_busy", return_value=False), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs", return_value=[(10, 100), (11, 101)]), \
             mock.patch.object(watcher, "reroute_to_local", return_value=True) as reroute, \
             mock.patch.object(watcher.time, "time", return_value=1000.0):
            watcher.tick(guard)
            reroute.assert_called_once_with(10)

        with mock.patch.object(watcher, "local_is_busy", return_value=False), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs", return_value=[(10, 102), (11, 103)]), \
             mock.patch.object(watcher, "reroute_to_local", return_value=True) as reroute, \
             mock.patch.object(watcher.time, "time", return_value=1001.0):
            watcher.tick(guard)
            reroute.assert_called_once_with(11)


if __name__ == "__main__":
    unittest.main()
