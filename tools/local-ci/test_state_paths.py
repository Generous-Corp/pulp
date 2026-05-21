#!/usr/bin/env python3
"""Tests for the state_paths state/log path helpers.

Split out of test_local_ci.py (roadmap P11-3) so the test surface mirrors
the extracted state_paths module. The harness still loads the local_ci.py
orchestrator, which re-exports the state_paths symbols.
"""

import io
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import urlparse
from unittest import mock
from contextlib import redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("local_ci.py")
VALIDATE_BUILD_PATH = MODULE_PATH.parent.parent.parent / "validate-build.sh"


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module



class StatePathsTests(unittest.TestCase):
    def _set_target_enabled(self, name: str, enabled: bool):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("targets", {}).setdefault(name, {})["enabled"] = enabled
        self.config_path.write_text(json.dumps(payload) + "\n")

    def _write_desktop_manifest(self, config, target, action, manifest):
        bundle = self.mod.create_desktop_run_bundle(config, target, action)
        payload = dict(manifest)
        artifacts = dict(payload.get("artifacts", {}))
        artifacts.setdefault("bundle_dir", str(bundle))
        payload["artifacts"] = artifacts
        (bundle / "manifest.json").write_text(json.dumps(payload) + "\n")
        return bundle, payload

    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
                    "desktop_automation": {
                        "artifact_root": str(root / "desktop-artifacts"),
                    },
                    "targets": {
                        "mac": {"type": "local", "enabled": True},
                        "ubuntu": {"type": "ssh", "enabled": True, "host": "ubuntu", "repo_path": "/tmp/pulp"},
                        "windows": {"type": "ssh", "enabled": False, "host": "win2", "repo_path": "C:\\Pulp"},
                    },
                    "github_actions": {
                        "repository": "danielraffel/pulp",
                        "defaults": {
                            "workflow": "build",
                            "provider": "github-hosted",
                            "wait_poll_secs": 5,
                            "match_timeout_secs": 30,
                        },
                        "workflows": {
                            "build": {
                                "providers": {
                                    "namespace": {
                                        "linux_runner_selector_json": "\"namespace-profile-default\"",
                                        "windows_runner_selector_json": "\"namespace-profile-default\"",
                                    }
                                }
                            },
                            "docs-check": {
                                "providers": {
                                    "namespace": {
                                        "runner_selector_json": "\"namespace-profile-default\""
                                    }
                                }
                            }
                        },
                    },
                    "defaults": {
                        "priority": "normal",
                        "targets": ["mac"],
                    },
                }
            )
            + "\n"
        )

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        self.prev_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        os.environ["PULP_LOCAL_CI_CONFIG"] = str(self.config_path)
        self.mod = load_module()

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
        state_paths_mod = sys.modules["state_paths"]

        with mock.patch.object(state_paths_mod, "SCRIPT_DIR", script_dir):
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.state_dir)}, clear=True):
                self.assertEqual(self.mod.config_path(), shared)
                shared.unlink()
                self.assertEqual(self.mod.config_path(), worktree)
                self.assertEqual(self.mod.worktree_config_path(), worktree)
                self.assertEqual(self.mod.shared_config_path(), shared)

    def test_state_paths_all_use_the_state_root(self):
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.state_dir)}, clear=True):
            self.assertEqual(self.mod.queue_path(), self.state_dir / "queue.json")
            self.assertEqual(self.mod.evidence_path(), self.state_dir / "evidence.json")
            self.assertEqual(self.mod.queue_lock_path(), self.state_dir / "queue.lock")
            self.assertEqual(self.mod.evidence_lock_path(), self.state_dir / "evidence.lock")
            self.assertEqual(self.mod.drain_lock_path(), self.state_dir / "drain.lock")
            self.assertEqual(self.mod.runner_info_path(), self.state_dir / "runner.json")
            self.assertEqual(self.mod.desktop_state_dir(), self.state_dir / "desktop-automation")
            self.assertEqual(
                self.mod.desktop_receipts_dir(),
                self.state_dir / "desktop-automation" / "receipts",
            )

    def test_state_dir_uses_platform_defaults_when_no_override_is_set(self):
        state_paths_mod = sys.modules["state_paths"]
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            state_paths_mod.Path,
            "home",
            return_value=Path("/Users/pulp"),
        ), mock.patch.object(state_paths_mod.sys, "platform", "darwin"):
            self.assertEqual(
                self.mod.state_dir(),
                Path("/Users/pulp/Library/Application Support/Pulp/local-ci"),
            )

        with mock.patch.dict(
            os.environ,
            {"XDG_STATE_HOME": "/xdg-state"},
            clear=True,
        ), mock.patch.object(state_paths_mod.sys, "platform", "linux"):
            self.assertEqual(self.mod.state_dir(), Path("/xdg-state/pulp/local-ci"))

        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            state_paths_mod.Path,
            "home",
            return_value=Path("/home/pulp"),
        ), mock.patch.object(state_paths_mod.sys, "platform", "linux"):
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



if __name__ == "__main__":
    unittest.main()
