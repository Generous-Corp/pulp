#!/usr/bin/env python3
"""Unit tests for tools/scripts/macos_reroute_watcher.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import runpy
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

        marker_without_build = (
            "/Users/runner/actions-runner/_work/pulp/pulp helper-process python\n"
        )
        with mock.patch.object(
            watcher.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["ps"], 0, stdout=marker_without_build),
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

    def test_tick_skips_all_flap_guarded_candidates(self) -> None:
        guard = watcher.FlapGuard(window_seconds=300)
        with mock.patch.object(watcher.time, "time", return_value=1000.0):
            guard.record(10)

        with mock.patch.object(watcher, "local_is_busy", return_value=False), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs", return_value=[(10, 100)]), \
             mock.patch.object(watcher, "reroute_to_local") as reroute, \
             mock.patch.object(watcher.time, "time", return_value=1001.0):
            watcher.tick(guard)

        reroute.assert_not_called()

    def test_tick_tries_next_candidate_when_reroute_fails(self) -> None:
        guard = watcher.FlapGuard(window_seconds=300)
        with mock.patch.object(watcher, "local_is_busy", return_value=False), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs", return_value=[(10, 100), (11, 101)]), \
             mock.patch.object(watcher, "reroute_to_local", side_effect=[False, True]) as reroute, \
             mock.patch.object(watcher.time, "time", return_value=2000.0):
            watcher.tick(guard)

        self.assertEqual([call.args[0] for call in reroute.call_args_list], [10, 11])
        self.assertNotIn(10, guard._last)
        self.assertIn(11, guard._last)

    def test_watch_exits_on_keyboard_interrupt_without_sleeping(self) -> None:
        with mock.patch.object(watcher, "tick", side_effect=KeyboardInterrupt) as tick, \
             mock.patch.object(watcher.time, "sleep") as sleep:
            watcher.watch(interval=7, flap_window=9)

        tick.assert_called_once()
        guard = tick.call_args.args[0]
        self.assertIsInstance(guard, watcher.FlapGuard)
        self.assertEqual(guard.window, 9)
        sleep.assert_not_called()

    def test_watch_logs_tick_errors_and_continues(self) -> None:
        with mock.patch.object(
            watcher,
            "tick",
            side_effect=[RuntimeError("boom"), KeyboardInterrupt],
        ) as tick, \
             mock.patch.object(watcher.time, "sleep") as sleep, \
             mock.patch.object(watcher.logging, "exception") as log_exception:
            watcher.watch(interval=3, flap_window=4)

        self.assertEqual(tick.call_count, 2)
        log_exception.assert_called_once()
        sleep.assert_called_once_with(3)

    def test_watch_sleeps_after_clean_tick_until_interrupt(self) -> None:
        with mock.patch.object(watcher, "tick", side_effect=[None, KeyboardInterrupt]) as tick, \
             mock.patch.object(watcher.time, "sleep") as sleep:
            watcher.watch(interval=11, flap_window=12)

        self.assertEqual(tick.call_count, 2)
        sleep.assert_called_once_with(11)

    def test_main_parses_args_and_invokes_watch(self) -> None:
        with mock.patch.object(watcher, "watch") as watch:
            self.assertEqual(
                watcher.main(["--interval", "5", "--flap-window", "6", "--log-level", "DEBUG"]),
                0,
            )

        watch.assert_called_once_with(
            interval=5, flap_window=6, hosts=watcher.DEFAULT_HOSTS
        )

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        argv = [str(SCRIPT), "--interval", "1"]
        with mock.patch.object(sys, "argv", argv), \
             mock.patch.object(subprocess, "run", side_effect=KeyboardInterrupt):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 0)


class CapacityModelTests(unittest.TestCase):
    """#3299 — VM-slot-aware free-capacity model."""

    def _tart_json(self, vms: list[dict]) -> subprocess.CompletedProcess:
        return subprocess.CompletedProcess(["tart"], 0, stdout=json.dumps(vms))

    def test_count_running_macos_vms_counts_only_running_macos(self) -> None:
        vms = [
            {"Name": "a", "State": "running", "OS": "darwin"},
            {"Name": "b", "State": "running", "OS": "linux"},   # uncapped → skip
            {"Name": "c", "State": "stopped", "OS": "darwin"},  # not running → skip
            {"Name": "d", "State": "running"},                  # unknown OS → counts
        ]
        with mock.patch.object(watcher.subprocess, "run",
                               return_value=self._tart_json(vms)) as run:
            self.assertEqual(watcher.count_running_macos_vms(), 2)
        # local invocation: no ssh wrapper
        self.assertEqual(run.call_args.args[0][:2], ["tart", "list"])

    def test_count_running_macos_vms_uses_ssh_for_remote_host(self) -> None:
        with mock.patch.object(watcher.subprocess, "run",
                               return_value=self._tart_json([])) as run:
            self.assertEqual(watcher.count_running_macos_vms(ssh="admin@m5"), 0)
        argv = run.call_args.args[0]
        self.assertEqual(argv[0], "ssh")
        self.assertIn("admin@m5", argv)
        self.assertEqual(argv[-1], "tart list --format json")

    def test_count_running_macos_vms_returns_none_on_failure(self) -> None:
        with mock.patch.object(watcher.subprocess, "run",
                               side_effect=subprocess.SubprocessError("boom")):
            self.assertIsNone(watcher.count_running_macos_vms())
        with mock.patch.object(
            watcher.subprocess, "run",
            return_value=subprocess.CompletedProcess(["tart"], 0, stdout="not json"),
        ):
            self.assertIsNone(watcher.count_running_macos_vms())

    def test_baremetal_host_free_slots_tracks_busy_probe(self) -> None:
        host = {"name": "local", "mode": "baremetal", "cap": 1}
        with mock.patch.object(watcher, "local_is_busy", return_value=False):
            self.assertEqual(watcher._host_free_slots(host), 1)
        with mock.patch.object(watcher, "local_is_busy", return_value=True):
            self.assertEqual(watcher._host_free_slots(host), 0)
        with mock.patch.object(watcher, "local_is_busy", return_value=None):
            self.assertIsNone(watcher._host_free_slots(host))

    def test_tart_host_free_slots_is_cap_minus_running(self) -> None:
        host = {"name": "studio", "mode": "tart", "cap": 2}
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=0):
            self.assertEqual(watcher._host_free_slots(host), 2)
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=2):
            self.assertEqual(watcher._host_free_slots(host), 0)
        # never negative even if a host somehow exceeds its cap
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=5):
            self.assertEqual(watcher._host_free_slots(host), 0)
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=None):
            self.assertIsNone(watcher._host_free_slots(host))

    def test_free_macos_slots_sums_hosts_and_tolerates_partial_failure(self) -> None:
        hosts = [
            {"name": "local", "mode": "baremetal", "cap": 1},
            {"name": "studio", "mode": "tart", "cap": 2},
            {"name": "m5", "mode": "tart", "cap": 2, "ssh": "admin@m5"},
        ]
        with mock.patch.object(watcher, "local_is_busy", return_value=False), \
             mock.patch.object(watcher, "count_running_macos_vms",
                               side_effect=[1, None]):  # studio: 1 free; m5: probe fails
            # 1 (baremetal idle) + 1 (studio 2-1) + skip(m5) = 2
            self.assertEqual(watcher.free_macos_slots(hosts), 2)

    def test_free_macos_slots_none_only_when_all_probes_fail(self) -> None:
        hosts = [{"name": "a", "mode": "tart", "cap": 2},
                 {"name": "b", "mode": "tart", "cap": 2}]
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=None):
            self.assertIsNone(watcher.free_macos_slots(hosts))

    def test_load_hosts_config_default_is_single_baremetal_slot(self) -> None:
        self.assertEqual(watcher.load_hosts_config(None), watcher.DEFAULT_HOSTS)
        with mock.patch.object(watcher, "local_is_busy", return_value=False):
            # Default config preserves the exact pre-#3299 behavior: 1 free iff idle.
            self.assertEqual(watcher.free_macos_slots(watcher.DEFAULT_HOSTS), 1)

    def test_load_hosts_config_reads_file_and_rejects_empty(self) -> None:
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            json.dump({"hosts": [{"name": "x", "mode": "tart", "cap": 2}]}, fh)
            good = fh.name
        self.assertEqual(watcher.load_hosts_config(good)[0]["name"], "x")
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            json.dump({"hosts": []}, fh)
            empty = fh.name
        with self.assertRaises(ValueError):
            watcher.load_hosts_config(empty)

    def test_tick_reroutes_when_tart_slot_free(self) -> None:
        guard = watcher.FlapGuard(window_seconds=300)
        hosts = [{"name": "studio", "mode": "tart", "cap": 2}]
        # 1 running VM, cap 2 → 1 free slot → should reroute a queued cloud job.
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=1), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs",
                               return_value=[(42, 400)]), \
             mock.patch.object(watcher, "reroute_to_local", return_value=True) as reroute:
            watcher.tick(guard, hosts)
        reroute.assert_called_once_with(42)

    def test_tick_skips_when_tart_host_at_cap(self) -> None:
        guard = watcher.FlapGuard(window_seconds=300)
        hosts = [{"name": "studio", "mode": "tart", "cap": 2}]
        with mock.patch.object(watcher, "count_running_macos_vms", return_value=2), \
             mock.patch.object(watcher, "list_queued_cloud_bat_runs") as queued:
            watcher.tick(guard, hosts)
        queued.assert_not_called()


if __name__ == "__main__":
    unittest.main()
