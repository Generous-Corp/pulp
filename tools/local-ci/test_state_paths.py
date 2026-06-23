#!/usr/bin/env python3
"""Tests for the state_paths state/log path helpers."""

import json
import os
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import state_paths



class StatePathsTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text("{}\n")

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        self.prev_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        os.environ["PULP_LOCAL_CI_CONFIG"] = str(self.config_path)
        self.mod = state_paths

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        if self.prev_config is None:
            os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        else:
            os.environ["PULP_LOCAL_CI_CONFIG"] = self.prev_config

        self.tmpdir.cleanup()


    def test_target_log_path_uses_machine_global_logs_dir(self):
        path = self.mod.target_log_path("job123", "windows")
        self.assertEqual(path, self.state_dir / "logs" / "job123" / "windows.log")

    def test_config_path_prefers_explicit_env_override(self):
        override = self.state_dir / "override.json"
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_CONFIG": str(override)}, clear=True):
            self.assertEqual(self.mod.config_path(), override)

    def test_config_path_prefers_shared_state_then_worktree_fallback(self):
        script_dir = Path(self.tmpdir.name) / "script"
        shared = self.state_dir / "config.json"
        worktree = script_dir / "config.json"
        script_dir.mkdir()
        shared.parent.mkdir(parents=True)
        shared.write_text("{}\n")
        with mock.patch.object(state_paths, "SCRIPT_DIR", script_dir):
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.state_dir)}, clear=True):
                self.assertEqual(self.mod.config_path(), shared)
                shared.unlink()
                self.assertEqual(self.mod.config_path(), worktree)
                self.assertEqual(self.mod.worktree_config_path(), worktree)
                self.assertEqual(self.mod.shared_config_path(), shared)

    def test_state_paths_all_use_the_state_root(self):
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.state_dir)}, clear=True):
            self.assertEqual(self.mod.queue_path(), self.state_dir / "queue.json")
            self.assertEqual(self.mod.results_dir(), self.state_dir / "results")
            self.assertEqual(self.mod.cloud_runs_dir(), self.state_dir / "cloud-runs")
            self.assertEqual(self.mod.evidence_path(), self.state_dir / "evidence.json")
            self.assertEqual(self.mod.logs_dir(), self.state_dir / "logs")
            self.assertEqual(self.mod.bundles_dir(), self.state_dir / "bundles")
            self.assertEqual(self.mod.prepared_dir(), self.state_dir / "prepared")
            self.assertEqual(self.mod.queue_lock_path(), self.state_dir / "queue.lock")
            self.assertEqual(self.mod.evidence_lock_path(), self.state_dir / "evidence.lock")
            self.assertEqual(self.mod.drain_lock_path(), self.state_dir / "drain.lock")
            self.assertEqual(self.mod.runner_info_path(), self.state_dir / "runner.json")
            self.assertEqual(self.mod.job_logs_dir("job789"), self.state_dir / "logs" / "job789")
            self.assertEqual(self.mod.desktop_state_dir(), self.state_dir / "desktop-automation")
            self.assertEqual(
                self.mod.desktop_receipts_dir(),
                self.state_dir / "desktop-automation" / "receipts",
            )

    def test_state_dir_uses_platform_defaults_when_no_override_is_set(self):
        with mock.patch.dict(os.environ, {"HOME": "/Users/pulp", "PULP_LOCAL_CI_HOME": "~/ci-state"}, clear=True):
            self.assertEqual(self.mod.state_dir(), Path("/Users/pulp/ci-state"))

        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            state_paths.Path,
            "home",
            return_value=Path("/Users/pulp"),
        ), mock.patch.object(state_paths.sys, "platform", "darwin"):
            self.assertEqual(
                self.mod.state_dir(),
                Path("/Users/pulp/Library/Application Support/Pulp/local-ci"),
            )

        with mock.patch.dict(
            os.environ,
            {"XDG_STATE_HOME": "/xdg-state"},
            clear=True,
        ), mock.patch.object(state_paths.sys, "platform", "linux"):
            self.assertEqual(self.mod.state_dir(), Path("/xdg-state/pulp/local-ci"))

        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            state_paths.Path,
            "home",
            return_value=Path("/home/pulp"),
        ), mock.patch.object(state_paths.sys, "platform", "linux"):
            self.assertEqual(self.mod.state_dir(), Path("/home/pulp/.local/state/pulp/local-ci"))

    def test_ensure_state_dirs_creates_shared_state_directories(self):
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.state_dir)}, clear=True):
            self.mod.ensure_state_dirs()

            for path in (
                self.state_dir,
                self.state_dir / "results",
                self.state_dir / "cloud-runs",
                self.state_dir / "logs",
                self.state_dir / "bundles",
                self.state_dir / "desktop-automation",
                self.state_dir / "desktop-automation" / "receipts",
            ):
                self.assertTrue(path.is_dir(), path)

    def test_prepare_target_log_creates_parent_and_truncates_existing_log(self):
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.state_dir)}, clear=True):
            path = self.mod.target_log_path("job456", "mac")
            path.parent.mkdir(parents=True)
            path.write_text("old output")

            prepared = self.mod.prepare_target_log("job456", "mac")

            self.assertEqual(prepared, path)
            self.assertEqual(prepared.read_text(), "")
            self.assertTrue(prepared.parent.is_dir())
            self.assertEqual(prepared.name, "mac.log")



if __name__ == "__main__":
    unittest.main()
