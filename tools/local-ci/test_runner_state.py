#!/usr/bin/env python3
"""Tests for runner_state helpers."""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("runner_state.py")


def load_module():
    script_dir = str(MODULE_PATH.parent)
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    spec = importlib.util.spec_from_file_location("pulp_runner_state", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class RunnerStateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_read_write_and_clear_runner_info(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "runner-info.json"

            self.assertIsNone(self.mod.read_runner_info(path))
            self.mod.write_runner_info({"pid": 123, "active_job_id": "job123"}, path)
            self.assertEqual(self.mod.read_runner_info(path), {"pid": 123, "active_job_id": "job123"})

            self.mod.clear_runner_info(path)
            self.assertIsNone(self.mod.read_runner_info(path))

    def test_pid_alive_handles_empty_invalid_and_current_pid(self) -> None:
        self.assertFalse(self.mod.pid_alive(None))
        self.assertFalse(self.mod.pid_alive(0))
        self.assertFalse(self.mod.pid_alive(-1))
        self.assertTrue(self.mod.pid_alive(os.getpid()))

    def test_current_runner_info_keeps_live_pid_and_removes_dead_pid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            info_path = Path(tmp) / "runner-info.json"
            lock_path = Path(tmp) / "drain.lock"
            self.mod.write_runner_info({"pid": 111, "active_job_id": "job123"}, info_path)

            self.assertEqual(
                self.mod.current_runner_info(
                    info_path=info_path,
                    lock_path=lock_path,
                    pid_alive_fn=lambda pid: True,
                ),
                {"pid": 111, "active_job_id": "job123"},
            )

            self.assertIsNone(
                self.mod.current_runner_info(
                    info_path=info_path,
                    lock_path=lock_path,
                    pid_alive_fn=lambda pid: False,
                )
            )
            self.assertFalse(info_path.exists())

    def test_current_runner_info_preserves_dead_pid_when_drain_lock_busy(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            info_path = Path(tmp) / "runner-info.json"
            lock_path = Path(tmp) / "drain.lock"
            info = {"pid": 111, "active_job_id": "job123"}
            self.mod.write_runner_info(info, info_path)

            def busy_lock(_path, *, blocking):
                raise self.mod.LockBusyError("busy")

            self.assertEqual(
                self.mod.current_runner_info(
                    info_path=info_path,
                    lock_path=lock_path,
                    pid_alive_fn=lambda pid: False,
                    file_lock_fn=busy_lock,
                ),
                info,
            )
            self.assertTrue(info_path.exists())

    def test_stale_running_jobs_for_current_runner_uses_live_runner_pid(self) -> None:
        queue = [{"id": "current", "status": "running", "runner": {"pid": 123}}]
        pid_calls: list[int | None] = []
        clear_calls: list[bool] = []
        selector_calls: list[tuple[list[dict], int | None]] = []
        stale_jobs = [{"id": "stale"}]

        def select(loaded_queue: list[dict], runner_pid: int | None) -> list[dict]:
            selector_calls.append((loaded_queue, runner_pid))
            return stale_jobs

        result = self.mod.stale_running_jobs_for_current_runner(
            queue,
            read_runner_info_fn=lambda: {"pid": 123, "active_job_id": "job123"},
            pid_alive_fn=lambda pid: pid_calls.append(pid) or True,
            clear_runner_info_fn=lambda: clear_calls.append(True),
            stale_running_jobs_for_runner_unlocked_fn=select,
        )

        self.assertIs(result, stale_jobs)
        self.assertEqual(pid_calls, [123])
        self.assertEqual(clear_calls, [])
        self.assertEqual(selector_calls, [(queue, 123)])

    def test_stale_running_jobs_for_current_runner_clears_dead_runner_pid(self) -> None:
        queue = [{"id": "old", "status": "running", "runner": {"pid": 456}}]
        clear_calls: list[bool] = []
        selector_calls: list[tuple[list[dict], int | None]] = []

        def select(loaded_queue: list[dict], runner_pid: int | None) -> list[dict]:
            selector_calls.append((loaded_queue, runner_pid))
            return loaded_queue

        result = self.mod.stale_running_jobs_for_current_runner(
            queue,
            read_runner_info_fn=lambda: {"pid": 456, "active_job_id": "job-old"},
            pid_alive_fn=lambda pid: False,
            clear_runner_info_fn=lambda: clear_calls.append(True),
            stale_running_jobs_for_runner_unlocked_fn=select,
        )

        self.assertIs(result, queue)
        self.assertEqual(clear_calls, [True])
        self.assertEqual(selector_calls, [(queue, None)])

    def test_stale_running_jobs_for_current_runner_handles_missing_runner_info(self) -> None:
        queue = [{"id": "old", "status": "running", "runner": {"pid": 456}}]
        pid_calls: list[int | None] = []
        clear_calls: list[bool] = []
        selector_calls: list[tuple[list[dict], int | None]] = []

        def select(loaded_queue: list[dict], runner_pid: int | None) -> list[dict]:
            selector_calls.append((loaded_queue, runner_pid))
            return []

        self.mod.stale_running_jobs_for_current_runner(
            queue,
            read_runner_info_fn=lambda: None,
            pid_alive_fn=lambda pid: pid_calls.append(pid) or False,
            clear_runner_info_fn=lambda: clear_calls.append(True),
            stale_running_jobs_for_runner_unlocked_fn=select,
        )

        self.assertEqual(pid_calls, [None])
        self.assertEqual(clear_calls, [])
        self.assertEqual(selector_calls, [(queue, None)])


if __name__ == "__main__":
    unittest.main()
