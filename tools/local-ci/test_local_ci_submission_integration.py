#!/usr/bin/env python3
"""Command-level local CI submission integration tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_submission_integration", add_module_dir=True)


class LocalCiSubmissionIntegrationTests(unittest.TestCase):
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
                            },
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

    def test_resolve_submission_options_uses_branch_tip_when_branch_is_explicit(self):
        args = SimpleNamespace(
            branch="feature/topic",
            sha=None,
            targets=None,
            priority=None,
            smoke=False,
            allow_root_mismatch=True,
            allow_unreachable_targets=False,
        )

        original_load_config = self.mod.load_config
        original_resolve_targets = self.mod.resolve_targets
        original_default_priority = self.mod.default_priority_for
        original_resolve_ref = self.mod.resolve_git_ref_sha
        original_current_sha = self.mod.current_sha

        self.mod.load_config = lambda: {"targets": {"mac": {"type": "local", "enabled": True}}, "defaults": {}}
        self.mod.resolve_targets = lambda config, requested: ["mac"]
        self.mod.default_priority_for = lambda command, config: "normal"
        self.mod.resolve_git_ref_sha = lambda ref: "b" * 40
        self.mod.current_sha = lambda: "a" * 40
        try:
            _config, branch, sha, targets, priority, validation, submission = self.mod.resolve_submission_options(
                args, "run"
            )
        finally:
            self.mod.load_config = original_load_config
            self.mod.resolve_targets = original_resolve_targets
            self.mod.default_priority_for = original_default_priority
            self.mod.resolve_git_ref_sha = original_resolve_ref
            self.mod.current_sha = original_current_sha

        self.assertEqual(branch, "feature/topic")
        self.assertEqual(sha, "b" * 40)
        self.assertEqual(targets, ["mac"])
        self.assertEqual(priority, "normal")
        self.assertEqual(validation, "full")
        self.assertEqual(submission["branch"], "feature/topic")
        self.assertEqual(Path(submission["config_path"]).resolve(), self.config_path.resolve())

    def test_build_submission_metadata_rejects_root_mismatch_by_default(self):
        config = self.mod.load_config()
        original_root = self.mod.ROOT
        original_git_root = self.mod.git_root_for
        self.mod.ROOT = Path("/tmp/pulp-root")
        self.mod.git_root_for = lambda path: Path("/tmp/other-root")
        try:
            with self.assertRaises(ValueError):
                self.mod.build_submission_metadata(
                    config,
                    "feature/topic",
                    "a" * 40,
                    ["mac"],
                    "normal",
                    "full",
                    allow_root_mismatch=False,
                    allow_unreachable_targets=False,
                )
        finally:
            self.mod.ROOT = original_root
            self.mod.git_root_for = original_git_root

    def test_build_submission_metadata_records_fallback_host_preflight(self):
        config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "fallback_host": "win",
                    "repo_path": "C:\\Pulp",
                }
            },
            "defaults": {},
        }
        original_ssh = self.mod.ssh_reachable
        self.mod.ssh_reachable = lambda host, timeout=5: host == "win"
        try:
            submission = self.mod.build_submission_metadata(
                config,
                "feature/topic",
                "a" * 40,
                ["windows"],
                "normal",
                "full",
                allow_root_mismatch=True,
                allow_unreachable_targets=False,
            )
        finally:
            self.mod.ssh_reachable = original_ssh

        state = submission["target_hosts"]["windows"]
        self.assertEqual(state["status"], "fallback-up")
        self.assertEqual(state["resolved_host"], "win")
        self.assertIn("fallback", submission["warnings"][0])

    def test_build_submission_metadata_fails_fast_for_unreachable_target_without_override(self):
        config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "repo_path": "C:\\Pulp",
                }
            },
            "defaults": {},
        }
        original_ssh = self.mod.ssh_reachable
        self.mod.ssh_reachable = lambda host, timeout=5: False
        try:
            with self.assertRaises(ValueError):
                self.mod.build_submission_metadata(
                    config,
                    "feature/topic",
                    "a" * 40,
                    ["windows"],
                    "normal",
                    "full",
                    allow_root_mismatch=True,
                    allow_unreachable_targets=False,
                )
        finally:
            self.mod.ssh_reachable = original_ssh

    def test_process_job_prefers_submission_config_path_for_windows_target(self):
        shared_config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win",
                    "repo_path": r"C:\SharedPulp",
                    "cmake_generator": "Visual Studio 17 2022",
                }
            },
            "defaults": {},
        }
        submitted_config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "repo_path": r"C:\WorktreePulp",
                    "cmake_generator": "Visual Studio 17 2022",
                }
            },
            "defaults": {},
        }
        submitted_path = Path(self.tmpdir.name) / "submitted-config.json"
        submitted_path.write_text(json.dumps(submitted_config) + "\n")
        job = self.mod.make_job(
            "feature/topic",
            "a" * 40,
            "normal",
            ["windows"],
            "run",
            "full",
            submission={
                "config_path": str(submitted_path),
                "target_hosts": {
                    "windows": {
                        "target": "windows",
                        "transport_mode": "bundle",
                        "configured_host": "win2",
                        "resolved_host": "win2",
                        "repo_path": r"C:\WorktreePulp",
                        "status": "primary-up",
                    }
                },
            },
        )

        captured = {}
        original_run_windows = self.mod.run_windows_ssh_validation
        self.mod.run_windows_ssh_validation = lambda target_name, host, repo_path, queued_job, **kwargs: {
            "target": target_name,
            "status": "pass",
            "exit_code": 0,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": "",
            "log_file": "",
            "host": captured.setdefault("host", host),
            "repo_path": captured.setdefault("repo_path", repo_path),
        }
        try:
            result = self.mod.process_job(job, shared_config)
        finally:
            self.mod.run_windows_ssh_validation = original_run_windows

        self.assertEqual(result["overall"], "pass")
        self.assertEqual(captured["host"], "win2")
        self.assertEqual(captured["repo_path"], r"C:\WorktreePulp")

    def test_build_target_tasks_binds_repo_paths_per_target(self):
        config = {
            "targets": {
                "ubuntu": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "ubuntu",
                    "repo_path": "/home/daniel/Code/pulp-validate",
                },
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win",
                    "repo_path": r"C:\Users\danielraffel\pulp-validate",
                    "cmake_generator": "Visual Studio 17 2022",
                },
            },
            "defaults": {},
        }
        job = self.mod.make_job(
            "feature/topic",
            "a" * 40,
            "normal",
            ["ubuntu", "windows"],
            "run",
            "full",
        )
        captured = {}
        original_host = self.mod.ensure_host_reachable
        original_posix = self.mod.run_posix_ssh_validation
        original_windows = self.mod.run_windows_ssh_validation
        self.mod.ensure_host_reachable = lambda target_name, target_cfg, defaults: target_cfg["host"]
        self.mod.run_posix_ssh_validation = lambda target_name, host, repo_path, queued_job, **kwargs: {
            "target": target_name,
            "status": "pass",
            "exit_code": 0,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": "",
            "log_file": "",
            "repo_path": captured.setdefault("ubuntu_repo_path", repo_path),
        }
        self.mod.run_windows_ssh_validation = lambda target_name, host, repo_path, queued_job, **kwargs: {
            "target": target_name,
            "status": "pass",
            "exit_code": 0,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": "",
            "log_file": "",
            "repo_path": captured.setdefault("windows_repo_path", repo_path),
        }
        try:
            tasks = self.mod._build_target_tasks(job, config)
            for _name, fn in tasks:
                fn()
        finally:
            self.mod.ensure_host_reachable = original_host
            self.mod.run_posix_ssh_validation = original_posix
            self.mod.run_windows_ssh_validation = original_windows

        self.assertEqual(captured["ubuntu_repo_path"], "/home/daniel/Code/pulp-validate")
        self.assertEqual(captured["windows_repo_path"], r"C:\Users\danielraffel\pulp-validate")


if __name__ == "__main__":
    unittest.main()
