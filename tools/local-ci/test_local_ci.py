#!/usr/bin/env python3

import importlib
import io
import json
import os
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import urlparse
from unittest import mock
from contextlib import redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("local_ci.py")
VALIDATE_BUILD_PATH = MODULE_PATH.parent.parent.parent / "validate-build.sh"


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci", add_module_dir=True)


class LocalCiTests(unittest.TestCase):
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
        # R2-1 (#2645): the cloud helpers (gh_*/nsc_*/cmd_cloud_*/billing) moved
        # to cloud.py. local_ci re-exports them, but cmd_cloud_* resolve their
        # helper calls in the cloud namespace, so monkeypatches must target the
        # cloud module — not the re-exported local_ci attribute.
        self.cloud = importlib.import_module("cloud")

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

    def test_enqueue_deduplicates_and_raises_priority(self):
        first, created_first = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "low",
            ["mac"],
            "run",
            "full",
        )
        second, created_second = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "high",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(created_first)
        self.assertFalse(created_second)
        self.assertEqual(first["id"], second["id"])

        stored = self.mod.load_job(first["id"])
        self.assertIsNotNone(stored)
        self.assertEqual(stored["priority"], "high")

    def test_desktop_target_overrides_replace_host_and_repo_path(self):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("desktop_automation", {}).setdefault("targets", {}).setdefault("windows", {}).update(
            {
                "enabled": True,
                "host": "win",
                "repo_path": r"C:\Users\daniel\Code\pulp-validate",
            }
        )
        self.config_path.write_text(json.dumps(payload) + "\n")

        config = self.mod.load_config()
        target = self.mod.resolve_desktop_target(config, "windows")

        self.assertEqual(target["host"], "win")
        self.assertEqual(target["repo_path"], r"C:\Users\daniel\Code\pulp-validate")

    def test_enqueue_treats_smoke_and_full_as_distinct_jobs(self):
        smoke_job, smoke_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac"],
            "run",
            "smoke",
        )
        full_job, full_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(smoke_created)
        self.assertTrue(full_created)
        self.assertNotEqual(smoke_job["id"], full_job["id"])
        self.assertEqual(smoke_job["validation"], "smoke")
        self.assertEqual(full_job["validation"], "full")

    def test_enqueue_supersedes_older_pending_same_scope(self):
        older_job, older_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )
        newer_job, newer_created = self.mod.enqueue_job(
            "feature/test",
            "b" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(older_created)
        self.assertTrue(newer_created)

        queue = self.mod.load_queue()
        older_stored = next(job for job in queue if job["id"] == older_job["id"])
        newer_stored = next(job for job in queue if job["id"] == newer_job["id"])

        self.assertEqual(older_stored["status"], "completed")
        self.assertEqual(older_stored["overall"], "superseded")
        self.assertEqual(older_stored["superseded_by"], newer_job["id"])
        self.assertEqual(newer_stored["status"], "pending")

        result = self.cloud.load_result(Path(older_stored["result_file"]))
        self.assertEqual(result["overall"], "superseded")
        self.assertEqual(result["superseded_by"], newer_job["id"])

    def test_enqueue_supersedes_broader_pending_same_sha_with_narrower_scope(self):
        broader_job, broader_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac", "windows"],
            "run",
            "smoke",
        )
        narrower_job, narrower_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["windows"],
            "run",
            "smoke",
        )

        self.assertTrue(broader_created)
        self.assertTrue(narrower_created)

        queue = self.mod.load_queue()
        broader_stored = next(job for job in queue if job["id"] == broader_job["id"])
        narrower_stored = next(job for job in queue if job["id"] == narrower_job["id"])

        self.assertEqual(broader_stored["status"], "completed")
        self.assertEqual(broader_stored["overall"], "superseded")
        self.assertEqual(broader_stored["superseded_by"], narrower_job["id"])
        self.assertEqual(broader_stored["superseded_reason"], "narrower_scope_queued")
        self.assertEqual(narrower_stored["status"], "pending")

    def test_claim_next_job_prefers_higher_priority(self):
        low_job, _ = self.mod.enqueue_job("feature/low", "1" * 40, "low", ["mac"], "run", "full")
        high_job, _ = self.mod.enqueue_job("feature/high", "2" * 40, "high", ["mac"], "run", "full")

        claimed = self.mod.claim_next_job()
        self.assertIsNotNone(claimed)
        self.assertEqual(claimed["id"], high_job["id"])
        self.assertNotEqual(claimed["id"], low_job["id"])

    def test_cancel_pending_job_marks_it_completed_with_canceled_result(self):
        job, created = self.mod.enqueue_job("feature/cancel", "5" * 40, "normal", ["ubuntu"], "run", "full")
        self.assertTrue(created)

        exit_code = self.mod.cmd_cancel(SimpleNamespace(job=job["id"]))
        self.assertEqual(exit_code, 0)

        stored = self.mod.load_job(job["id"])
        self.assertIsNotNone(stored)
        self.assertEqual(stored["status"], "completed")
        self.assertEqual(stored["overall"], "canceled")
        result = self.cloud.load_result(Path(stored["result_file"]))
        self.assertEqual(result["overall"], "canceled")
        self.assertEqual(result["canceled_reason"], "operator_canceled")

    def test_config_path_prefers_shared_state_config(self):
        original_override = os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        shared_config = self.state_dir / "config.json"
        shared_config.parent.mkdir(parents=True, exist_ok=True)
        shared_config.write_text(
            json.dumps(
                {
                    "targets": {"mac": {"type": "local", "enabled": True}},
                    "defaults": {"priority": "normal", "targets": ["mac"]},
                }
            )
            + "\n"
        )
        try:
            self.assertEqual(self.mod.config_path(), shared_config)
        finally:
            if original_override is None:
                os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
            else:
                os.environ["PULP_LOCAL_CI_CONFIG"] = original_override

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

    def test_load_config_normalizes_desktop_automation_defaults(self):
        config = self.mod.load_config()

        desktop = config["desktop_automation"]
        self.assertEqual(desktop["publish_mode"], "none")
        self.assertEqual(desktop["publish_branch"], "dev-artifacts")
        self.assertEqual(desktop["retention_days"], 14)
        self.assertTrue(desktop["artifact_root"])
        self.assertEqual(desktop["targets"]["mac"]["adapter"], "macos-local")
        self.assertEqual(desktop["targets"]["mac"]["capability_tier"], "v2")
        self.assertEqual(desktop["targets"]["ubuntu"]["adapter"], "linux-xvfb")
        self.assertEqual(desktop["targets"]["windows"]["adapter"], "windows-session-agent")
        self.assertEqual(desktop["targets"]["windows"]["capability_tier"], "v2")
        self.assertEqual(desktop["targets"]["windows"]["task_name"], None)
        self.assertEqual(desktop["targets"]["windows"]["remote_root"], None)
        contract = self.mod.desktop_target_contract("windows", desktop["targets"]["windows"])
        self.assertEqual(contract["task_name"], "PulpDesktopAutomationAgent-windows")
        self.assertTrue(contract["remote_root"].startswith("%LOCALAPPDATA%"))

    def test_build_windows_session_agent_request_sets_outputs_and_env(self):
        config = self.mod.load_config()
        contract = self.mod.desktop_target_contract("windows", config["desktop_automation"]["targets"]["windows"])
        request = self.mod.build_windows_session_agent_request(
            "windows",
            contract,
            r"C:\Pulp\build\ui-preview.exe",
            repo_path=r"C:\Pulp",
            action_name="click",
            label="ui-preview",
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id="bypass-toggle",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.75,
            timeout_secs=20.0,
        )

        self.assertEqual(request["schema"], 1)
        self.assertEqual(request["target"], "windows")
        self.assertEqual(request["action"], "click")
        self.assertEqual(request["cwd"], r"C:\Pulp")
        self.assertEqual(request["execution"]["capture_mode"], "pulp-app")
        self.assertTrue(request["execution"]["capture_ui_snapshot"])
        self.assertTrue(request["execution"]["capture_before"])
        self.assertEqual(request["interaction"]["view_id"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        self.assertIn("\\results\\", request["outputs"]["screenshot"])
        self.assertIn("ui-tree.json", request["outputs"]["ui_snapshot"])
        self.assertIn("before.png", request["outputs"]["before_screenshot"])

    def test_make_desktop_source_request_defaults_to_live_current_branch_and_sha(self):
        args = SimpleNamespace(
            source_mode=None,
            branch=None,
            sha=None,
            prepare_command=None,
            prepare_timeout=None,
        )
        with mock.patch.object(self.mod, "current_branch", return_value="feature/test"):
            with mock.patch.object(self.mod, "current_sha", return_value="a" * 40):
                request = self.mod.make_desktop_source_request(args)

        self.assertEqual(request["mode"], "live")
        self.assertEqual(request["branch"], "feature/test")
        self.assertEqual(request["sha"], "a" * 40)
        self.assertEqual(request["prepare_timeout_secs"], 900.0)

    def test_rewrite_launch_command_helpers_retarget_repo_local_paths(self):
        command = f"{self.mod.ROOT}/build/ui-preview --flag"
        source_root = Path(self.tmpdir.name) / "prepared"

        local = self.mod.rewrite_launch_command_for_source_root(command, source_root)
        linux_remote = self.mod.rewrite_launch_command_for_posix_root(command, "$HOME/.local/state/pulp/source")
        windows_remote = self.mod.rewrite_launch_command_for_windows_root(command, r"C:\Users\daniel\AppData\Local\Pulp\source")

        self.assertIn(str(source_root / "build" / "ui-preview"), local)
        self.assertIn("$HOME/.local/state/pulp/source/build/ui-preview", linux_remote)
        self.assertIn(r"C:\Users\daniel\AppData\Local\Pulp\source\build\ui-preview", windows_remote)

    def test_rewrite_launch_command_for_windows_root_uses_windows_quoting(self):
        command = r'.\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe --flag'

        rewritten = self.mod.rewrite_launch_command_for_windows_root(command, r'C:\Program Files\Pulp\desktop-source\windows\abc123')

        self.assertIn(r'"C:\Program Files\Pulp\desktop-source\windows\abc123\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe" --flag', rewritten)
        self.assertNotIn("'", rewritten)

    def test_rewrite_launch_command_helpers_support_windows_relative_tokens(self):
        command = r".\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe --flag"

        rewritten = self.mod.rewrite_launch_command_for_windows_root(command, r"%LOCALAPPDATA%\Pulp\desktop-source\windows\abc123")

        self.assertIn(r"%LOCALAPPDATA%\Pulp\desktop-source\windows\abc123\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe", rewritten)

    def test_split_windows_prepare_commands_preserves_quoted_generator(self):
        commands = self.mod.split_windows_prepare_commands(
            'cmake -S . -B build -G "Visual Studio 17 2022" -A x64; cmake --build build --config Debug'
        )

        self.assertEqual(
            commands,
            [
                'cmake -S . -B build -G "Visual Studio 17 2022" -A x64',
                "cmake --build build --config Debug",
            ],
        )

    def test_validate_windows_prepare_commands_rejects_single_quoted_tokens(self):
        with self.assertRaises(ValueError) as ctx:
            self.mod.validate_windows_prepare_commands(
                ["cmake -S . -B build -G 'Visual Studio 17 2022' -A x64"]
            )

        self.assertIn("single-quoted tokens are literal text", str(ctx.exception))

    def test_validate_windows_prepare_commands_accepts_double_quoted_tokens(self):
        self.mod.validate_windows_prepare_commands(
            ['cmake -S . -B build -G "Visual Studio 17 2022" -A x64']
        )

    def test_prepare_linux_exact_sha_source_fetches_bundle_ref_without_lfs_smudge(self):
        bundle_dir = Path(self.tmpdir.name) / "bundle-linux"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "e" * 40,
            "prepare_timeout_secs": 120.0,
        }
        captured = {}

        def fake_run(cmd, **kwargs):
            captured["cmd"] = cmd
            return SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr="")

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("bundle.git", "refs/pulp-ci-bundles/test")):
            with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
                with mock.patch.object(self.mod, "fetch_ssh_artifact"):
                    context = self.mod.prepare_linux_exact_sha_source(
                        bundle_dir,
                        "ubuntu",
                        "ubuntu",
                        "./build/ui-preview --flag",
                        source_request,
                    )

        remote_script = captured["cmd"][-1]
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"', remote_script)
        self.assertIn("export GIT_LFS_SKIP_SMUDGE=1", remote_script)
        self.assertIn("bundle_ref=refs/pulp-ci-bundles/test", remote_script)
        self.assertIn('git -C "$prepared_root" init --quiet', remote_script)
        self.assertIn('git -C "$prepared_root" fetch "$bundle" "$bundle_ref:refs/pulp-ci-bundles/source" >/dev/null 2>&1', remote_script)
        self.assertIn('git -C "$prepared_root" remote add origin "$remote_url"', remote_script)
        self.assertEqual(context["prepared_state"], "clean")

    def test_prepare_linux_exact_sha_source_requires_prepare_stamp_when_prepare_command_exists(self):
        bundle_dir = Path(self.tmpdir.name) / "bundle-linux-prepare"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "f" * 40,
            "prepare_command": "./scripts/build-ui-preview.sh",
            "prepare_timeout_secs": 120.0,
        }
        captured = {}

        def fake_run(cmd, **kwargs):
            captured["cmd"] = cmd
            return SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr="")

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("bundle.git", "refs/pulp-ci-bundles/test")):
            with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
                with mock.patch.object(self.mod, "fetch_ssh_artifact"):
                    context = self.mod.prepare_linux_exact_sha_source(
                        bundle_dir,
                        "ubuntu",
                        "ubuntu",
                        "./build/ui-preview --flag",
                        source_request,
                    )

        remote_script = captured["cmd"][-1]
        self.assertIn("export PULP_REQUIRE_PREPARE_STAMP=1", remote_script)
        self.assertIn('if [ ! -f "$prepare_stamp" ] && [ -n "${PULP_REQUIRE_PREPARE_STAMP:-}" ]; then reused=0; else reused=1; fi', remote_script)
        self.assertIn("printf", remote_script)
        self.assertIn("> \"$prepare_stamp\"", remote_script)
        self.assertEqual(context["prepared_state"], "clean")

    def test_prepare_windows_exact_sha_source_expands_environment_aware_paths(self):
        bundle_dir = Path(self.tmpdir.name) / "bundle"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "d" * 40,
            "prepare_command": ".\\scripts\\build-ui-preview.ps1",
            "prepare_timeout_secs": 120.0,
        }
        scripts = []

        def fake_run_windows_ssh_powershell(host, ps_script, *, timeout=60):
            scripts.append(ps_script)
            return SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr="")

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("bundle.git", "refs/pulp-ci-bundles/test")):
            with mock.patch.object(self.mod, "run_windows_ssh_powershell", side_effect=fake_run_windows_ssh_powershell):
                with mock.patch.object(self.mod, "windows_ssh_fetch_file"):
                    context = self.mod.prepare_windows_exact_sha_source(
                        bundle_dir,
                        "windows",
                        "win",
                        r".\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe",
                        source_request,
                    )

        script = scripts[0]
        self.assertIn("$env:GIT_LFS_SKIP_SMUDGE = '1'", script)
        self.assertIn("$Bundle = Join-Path $HOME 'bundle.git'", script)
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/test'", script)
        self.assertIn("$PreparedRoot = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("$RemotePrepareLog = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("$PrepareStamp = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("$env:PULP_REQUIRE_PREPARE_STAMP = '1'", script)
        self.assertIn("$Sha = 'dddddddddddddddddddddddddddddddddddddddd'", script)
        self.assertIn("$PreparedHead = $null", script)
        self.assertIn("$PreparedHead = git -C $PreparedRoot rev-parse HEAD 2>$null", script)
        self.assertIn("if ($Reused -and $env:PULP_REQUIRE_PREPARE_STAMP -and -not (Test-Path $PrepareStamp)) { $Reused = $false }", script)
        self.assertIn('cmd.exe /c "rmdir /s /q \\"$PreparedRoot\\""', script)
        self.assertIn("git -C $PreparedRoot init --quiet | Out-Null", script)
        self.assertIn("git -C $PreparedRoot fetch $Bundle \"$BundleRef`:refs/pulp-ci-bundles/source\" | Out-Null", script)
        self.assertIn("Set-Content -LiteralPath $PrepareStamp -Value $Sha -Encoding UTF8", script)
        self.assertIn("$PrepareScriptPath = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("@'", script)
        self.assertIn("@echo off", script)
        self.assertIn("cd /d \"%~dp0\"", script)
        self.assertIn(".\\scripts\\build-ui-preview.ps1", script)
        self.assertIn("if errorlevel 1 exit /b %errorlevel%", script)
        self.assertIn("Set-Content -LiteralPath $PrepareScriptPath -Encoding UTF8", script)
        self.assertIn("$PrepareCmd = ('\"{0}\" > \"{1}\" 2>&1' -f $PrepareScriptPath, $RemotePrepareLog)", script)
        self.assertIn("cmd.exe /c $PrepareCmd | Out-Null", script)
        self.assertIn("Remove-Item -LiteralPath $PrepareScriptPath -Force", script)
        self.assertEqual(context["prepared_state"], "clean")
        self.assertIn(r"%LOCALAPPDATA%\Pulp\desktop-source\windows", context["launch_command"])

    def test_cmd_desktop_install_records_receipt(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        self.assertEqual(exit_code, 0)
        receipt = self.mod.desktop_receipt_for("ubuntu")
        self.assertIsNotNone(receipt)
        self.assertEqual(receipt["target"], "ubuntu")
        self.assertEqual(receipt["adapter"], "linux-xvfb")
        self.assertFalse(receipt["remote_bootstrap_ready"])
        self.assertTrue(Path(receipt["artifact_root"]).exists())
        self.assertIn("Desktop target `ubuntu` prepared.", buf.getvalue())

    def test_cmd_desktop_doctor_reports_remote_target_health(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "xvfb", "path": "/usr/bin/xvfb-run"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": True,
                        "git_lfs_version": "git-lfs/3.5.1",
                        "git_lfs_path": "/usr/bin/git-lfs",
                        "xvfb_run_found": True,
                        "xvfb_run_version": "Usage: xvfb-run [OPTION ...] COMMAND",
                        "xvfb_run_path": "/usr/bin/xvfb-run",
                        "xauth_found": True,
                        "xauth_version": "1.1.2",
                        "xauth_path": "/usr/bin/xauth",
                        "xdotool_found": True,
                        "xdotool_version": "xdotool version 3.20211022.1",
                        "xdotool_path": "/usr/bin/xdotool",
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": True,
                        "import_version": "Version: ImageMagick 6.9.12-98",
                        "import_path": "/usr/bin/import",
                        "wmctrl_found": True,
                        "wmctrl_version": "1.07",
                        "wmctrl_path": "/usr/bin/wmctrl",
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop doctor for `ubuntu`", output)
        self.assertIn("PASS  receipt: installed", output)
        self.assertIn("PASS  ssh: ubuntu", output)
        self.assertIn("PASS  launch_backend: /usr/bin/xvfb-run", output)
        self.assertIn("PASS  git-lfs: git-lfs/3.5.1 (/usr/bin/git-lfs)", output)
        self.assertIn("PASS  xdotool: xdotool version 3.20211022.1 (/usr/bin/xdotool)", output)

    def test_cmd_desktop_doctor_reports_linux_xvfb_remediation(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "missing"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": False,
                        "git_lfs_path": "/home/daniel/.local/bin/git-lfs",
                        "git_lfs_version": "git-lfs/3.7.0",
                        "git_lfs_hint": "installed at /home/daniel/.local/bin/git-lfs but unavailable to non-interactive shells; add $HOME/.local/bin to PATH or install git-lfs system-wide",
                        "xvfb_run_found": False,
                        "xauth_found": False,
                        "xdotool_found": False,
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": False,
                        "wmctrl_found": False,
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("install xvfb and xauth", output)
        self.assertIn("FAIL  git-lfs: installed at /home/daniel/.local/bin/git-lfs but unavailable to non-interactive shells", output)
        self.assertIn("FAIL  xdotool: missing; sudo apt-get install -y xdotool", output)
        self.assertIn("FAIL  import: missing; sudo apt-get install -y imagemagick", output)
        self.assertIn("WARN  wmctrl: missing; sudo apt-get install -y wmctrl", output)

    def test_cmd_desktop_doctor_reports_windows_ssh_handshake_reset(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "ensure_windows_remote_repo_checkout",
                return_value={
                    "home_dir": r"C:\Users\danielraffel",
                    "repo_path": r"C:\Users\danielraffel\pulp-validate",
                    "repo_exists": True,
                    "git_dir_exists": True,
                    "origin_url": "https://github.com/danielraffel/pulp",
                    "repo_path_unsafe": False,
                },
            ):
                with mock.patch.object(
                    self.mod.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                ):
                    with mock.patch.object(
                        self.mod,
                        "sync_job_bundle_to_ssh_host",
                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                    ):
                        self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=False):
            with mock.patch.object(
                self.mod,
                "ssh_failure_detail",
                return_value="win2 (SSH service reset during handshake; verify OpenSSH server on the target)",
            ):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL  ssh: win2 (SSH service reset during handshake", output)

    def test_cmd_desktop_doctor_accepts_existing_linux_display_session(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "display", "display": ":0", "xdg_runtime_dir": "/run/user/1000"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": True,
                        "git_lfs_version": "git-lfs/3.5.1",
                        "git_lfs_path": "/usr/bin/git-lfs",
                        "xvfb_run_found": True,
                        "xvfb_run_version": "Usage: xvfb-run [OPTION ...] COMMAND",
                        "xvfb_run_path": "/usr/bin/xvfb-run",
                        "xauth_found": True,
                        "xauth_version": "1.1.2",
                        "xauth_path": "/usr/bin/xauth",
                        "xdotool_found": True,
                        "xdotool_version": "xdotool version 3.20211022.1",
                        "xdotool_path": "/usr/bin/xdotool",
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": True,
                        "import_version": "Version: ImageMagick 6.9.12-98",
                        "import_path": "/usr/bin/import",
                        "wmctrl_found": True,
                        "wmctrl_version": "1.07",
                        "wmctrl_path": "/usr/bin/wmctrl",
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("PASS  launch_backend: existing display :0", output)


    def test_cmd_desktop_doctor_reports_windows_session_contract(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_name": "PulpDesktopAutomationAgent-windows",
                        "task_present": False,
                        "task_state": "",
                        "interactive_user": r"DESKTOP\daniel",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": False,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": False,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": False,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": False,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": False,
                                "winget_found": True,
                                "gh_auth_ready": None,
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with mock.patch.object(
                                    self.mod.subprocess,
                                    "run",
                                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                                ):
                                    with mock.patch.object(
                                        self.mod,
                                        "sync_job_bundle_to_ssh_host",
                                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                    ):
                                        self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))
                    with mock.patch.object(
                        self.mod,
                        "probe_windows_remote_tooling",
                        return_value={
                            "git_found": True,
                            "git_path": r"C:\Program Files\Git\cmd\git.exe",
                            "git_version": "git version 2.49.0.windows.1",
                            "gh_found": False,
                            "gh_path": "",
                            "gh_version": "",
                            "gh_auth_ready": None,
                            "gh_auth_detail": "",
                            "winget_found": True,
                            "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                            "winget_version": "v1.28.220",
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "probe_windows_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            buf = io.StringIO()
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with redirect_stdout(buf):
                                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop doctor for `windows`", output)
        self.assertIn("PASS  ssh: win", output)
        self.assertIn("WARN  scheduled_task: PulpDesktopAutomationAgent-windows (missing)", output)
        self.assertIn(r"PASS  interactive_user: DESKTOP\daniel", output)
        self.assertIn(r"PASS  git: git version 2.49.0.windows.1 (C:\Program Files\Git\cmd\git.exe)", output)
        self.assertIn("WARN  gh: missing; optional for remote GitHub workflows on the Windows target", output)
        self.assertIn(r"PASS  repo_checkout: C:\Users\danielraffel\pulp-validate (https://github.com/danielraffel/pulp)", output)

    def test_cmd_desktop_doctor_accepts_disconnected_windows_session(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_name": "PulpDesktopAutomationAgent-windows",
                        "task_present": True,
                        "task_state": "Ready",
                        "interactive_user": "",
                        "logged_on_user": "danielraffel",
                        "session_state": "Disc",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": True,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": True,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": True,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": True,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": False,
                                "winget_found": True,
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with mock.patch.object(
                                    self.mod.subprocess,
                                    "run",
                                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                                ):
                                    with mock.patch.object(
                                        self.mod,
                                        "sync_job_bundle_to_ssh_host",
                                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                    ):
                                        self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))
                    with mock.patch.object(
                        self.mod,
                        "probe_windows_remote_tooling",
                        return_value={
                            "git_found": True,
                            "git_path": r"C:\Program Files\Git\cmd\git.exe",
                            "git_version": "git version 2.49.0.windows.1",
                            "gh_found": False,
                            "gh_path": "",
                            "gh_version": "",
                            "gh_auth_ready": None,
                            "gh_auth_detail": "",
                            "winget_found": True,
                            "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                            "winget_version": "v1.28.220",
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "probe_windows_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            buf = io.StringIO()
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with redirect_stdout(buf):
                                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("PASS  interactive_user: danielraffel (Disc)", output)
    def test_cmd_desktop_doctor_treats_macos_accessibility_as_optional(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="mac"))

        with mock.patch.object(self.mod, "macos_accessibility_trusted", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("WARN  accessibility:", output)
        self.assertIn("Pulp app automation still works", output)

    def test_cmd_desktop_status_prints_target_summary(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="mac"))

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop automation:", output)
        self.assertIn("mac:", output)
        self.assertIn("adapter: macos-local", output)
        self.assertIn("installed: yes", output)
        self.assertIn("pulp_app_automation", output)

    def test_cmd_desktop_status_prints_windows_contract_summary(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_present": True,
                        "task_state": "Ready",
                        "interactive_user": r"DESKTOP\daniel",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": True,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": True,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": True,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": True,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": True,
                                "gh_path": r"C:\Program Files\GitHub CLI\gh.exe",
                                "gh_version": "gh version 2.70.0",
                                "gh_auth_ready": True,
                                "gh_auth_detail": "authenticated",
                                "winget_found": True,
                                "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                                "winget_version": "v1.28.220",
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(
                                self.mod.subprocess,
                                "run",
                                return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                            ):
                                with mock.patch.object(
                                    self.mod,
                                    "sync_job_bundle_to_ssh_host",
                                    return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                ):
                                    self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("task_name: PulpDesktopAutomationAgent-windows", output)
        self.assertIn(r"remote_root: C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent", output)
        self.assertIn("remote_bootstrap_ready: True", output)
        self.assertIn("remote_tooling_ready: True", output)
        self.assertIn("remote_repo_checkout_ready: True", output)
        self.assertIn(r"remote_git: git version 2.49.0.windows.1 (C:\Program Files\Git\cmd\git.exe)", output)
        self.assertIn(r"remote_gh: gh version 2.70.0 (C:\Program Files\GitHub CLI\gh.exe)", output)
        self.assertIn(r"remote_repo_checkout: C:\Users\danielraffel\pulp-validate (https://github.com/danielraffel/pulp)", output)

    def test_publish_report_to_branch_pushes_report_to_remote_branch(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / 'desktop-artifacts'
        config['desktop_automation']['artifact_root'] = str(artifact_root)
        config['desktop_automation']['publish_mode'] = 'branch'
        config['desktop_automation']['publish_branch'] = 'dev-artifacts-test'

        report_dir = artifact_root / '_published' / '20260404-branch-test'
        (report_dir / 'assets' / 'run-01').mkdir(parents=True, exist_ok=True)
        (report_dir / 'assets' / 'run-01' / 'window.png').write_bytes(b'png')
        (report_dir / 'index.json').write_text(json.dumps({'label': 'branch-gallery'}) + '\n')
        (report_dir / 'index.html').write_text('<html></html>\n')
        report = {
            'generated_at': '2026-04-04T21:30:00+00:00',
            'label': 'branch-gallery',
            'publish_mode': 'branch',
            'publish_branch': 'dev-artifacts-test',
            'output_dir': str(report_dir),
            'index_html': str(report_dir / 'index.html'),
            'index_json': str(report_dir / 'index.json'),
            'run_count': 1,
            'runs': [
                {
                    'label': 'ui-preview',
                    'target': 'mac',
                    'action': 'click',
                    'artifacts': {'screenshot': 'assets/run-01/window.png'},
                }
            ],
        }

        git_root = Path(self.tmpdir.name) / 'git-root'
        remote_root = Path(self.tmpdir.name) / 'remote.git'
        git_root.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', '--bare', str(remote_root)], check=True, capture_output=True, text=True)
        subprocess.run(['git', 'init'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'config', 'user.name', 'Pulp Tests'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'config', 'user.email', 'tests@example.com'], cwd=git_root, check=True, capture_output=True, text=True)
        (git_root / 'README.md').write_text('root\n')
        subprocess.run(['git', 'add', 'README.md'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'commit', '-m', 'Initial'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'remote', 'add', 'origin', str(remote_root)], cwd=git_root, check=True, capture_output=True, text=True)

        with mock.patch.object(self.mod, 'ROOT', git_root):
            published = self.mod.publish_report_to_branch(config, report)

        self.assertEqual(published['mode'], 'branch')
        self.assertEqual(published['branch'], 'dev-artifacts-test')
        clone_root = Path(self.tmpdir.name) / 'clone'
        subprocess.run(['git', 'clone', '--branch', 'dev-artifacts-test', str(remote_root), str(clone_root)], check=True, capture_output=True, text=True)
        self.assertTrue((clone_root / 'desktop-automation' / 'reports' / '20260404-branch-test' / 'index.json').exists())
        self.assertTrue((clone_root / 'desktop-automation' / 'latest' / 'index.html').exists())

    def test_stage_desktop_publish_report_includes_branch_publish_metadata_when_enabled(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / 'desktop-artifacts'
        config['desktop_automation']['artifact_root'] = str(artifact_root)
        config['desktop_automation']['publish_mode'] = 'branch'
        bundle = self.mod.create_desktop_run_bundle(config, 'mac', 'inspect')
        screenshot = bundle / 'screenshots' / 'window.png'
        screenshot.parent.mkdir(parents=True, exist_ok=True)
        screenshot.write_bytes(b'after')
        manifest = {
            'target': 'mac',
            'action': 'inspect',
            'label': 'ui-preview',
            'completed_at': '2026-04-04T06:30:00+00:00',
            'artifacts': {'bundle_dir': str(bundle), 'screenshot': str(screenshot)},
        }
        (bundle / 'manifest.json').write_text(json.dumps(manifest) + '\n')

        with mock.patch.object(self.mod, 'publish_report_to_branch', return_value={'mode': 'branch', 'branch': 'dev-artifacts'}):
            report = self.mod.stage_desktop_publish_report(config, [manifest], label='desktop-gallery')

        self.assertEqual(report['published']['branch'], 'dev-artifacts')

    def test_cmd_desktop_cleanup_removes_old_bundles(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        keep_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        remove_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        keep_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "keep",
            "completed_at": "2026-04-04T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(keep_bundle)},
        }
        remove_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "remove",
            "completed_at": "2026-04-01T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(remove_bundle)},
        }
        (keep_bundle / "manifest.json").write_text(json.dumps(keep_manifest) + "\n")
        (remove_bundle / "manifest.json").write_text(json.dumps(remove_manifest) + "\n")

        original_time = self.mod.time.time
        self.mod.time.time = lambda: self.mod.datetime.fromisoformat("2026-04-04T06:31:00+00:00").timestamp()
        try:
            with mock.patch.object(self.mod, "load_config", return_value=config):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_cleanup(
                        SimpleNamespace(target="mac", older_than_days=1, keep_last=1)
                    )
        finally:
            self.mod.time.time = original_time

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop cleanup removed 1 bundle(s).", output)
        self.assertTrue(keep_bundle.exists())
        self.assertFalse(remove_bundle.exists())
        latest_run = json.loads((artifact_root / "mac" / "latest-run.json").read_text())
        runs_jsonl = (artifact_root / "mac" / "runs.jsonl").read_text().strip().splitlines()
        self.assertEqual(latest_run["label"], "keep")
        self.assertEqual(len(runs_jsonl), 1)

    def test_stale_running_job_requeues_when_runner_dies(self):
        job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["mac"], "run", "full")
        job["status"] = "running"
        job["started_at"] = "2026-03-31T00:00:00+00:00"
        job["runner"] = {"pid": 999999, "root": "/tmp/pulp"}
        job["active_targets"] = {
            "mac": {"status": "pass", "duration_secs": 10.0},
            "windows": {"status": "running"},
        }

        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([job], indent=2) + "\n")
        self.mod.runner_info_path().write_text(
            json.dumps({"pid": 999999, "active_job_id": job["id"], "active_branch": job["branch"]}) + "\n"
        )

        queue = self.mod.load_queue()
        self.assertEqual(queue[0]["status"], "pending")
        self.assertIn("requeued_at", queue[0])
        self.assertEqual(queue[0]["active_targets"]["mac"]["status"], "pass")
        self.assertEqual(queue[0]["active_targets"]["windows"]["status"], "running")
        self.assertFalse(self.mod.runner_info_path().exists())

    def test_stale_running_job_is_superseded_when_newer_pending_exists(self):
        older_job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["mac"], "run", "full")
        older_job["queued_at"] = "2026-03-31T00:00:00+00:00"
        older_job["status"] = "running"
        older_job["started_at"] = "2026-03-31T00:10:00+00:00"
        older_job["runner"] = {"pid": 999999, "root": "/tmp/pulp"}

        newer_job = self.mod.make_job("feature/stale", "4" * 40, "high", ["mac"], "run", "full")
        newer_job["queued_at"] = "2026-03-31T00:20:00+00:00"

        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([older_job, newer_job], indent=2) + "\n")
        self.mod.runner_info_path().write_text(
            json.dumps({"pid": 999999, "active_job_id": older_job["id"], "active_branch": older_job["branch"]}) + "\n"
        )

        queue = self.mod.load_queue()
        older_stored = next(job for job in queue if job["id"] == older_job["id"])
        newer_stored = next(job for job in queue if job["id"] == newer_job["id"])

        self.assertEqual(older_stored["status"], "completed")
        self.assertEqual(older_stored["overall"], "superseded")
        self.assertEqual(older_stored["superseded_by"], newer_job["id"])
        self.assertEqual(newer_stored["status"], "pending")

    def test_stale_running_broader_job_is_superseded_by_narrower_same_sha_scope(self):
        broader_job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["mac", "windows"], "run", "smoke")
        broader_job["queued_at"] = "2026-03-31T00:00:00+00:00"
        broader_job["status"] = "running"
        broader_job["started_at"] = "2026-03-31T00:10:00+00:00"
        broader_job["runner"] = {"pid": 999999, "root": "/tmp/pulp"}

        narrower_job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["windows"], "run", "smoke")
        narrower_job["queued_at"] = "2026-03-31T00:20:00+00:00"

        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([broader_job, narrower_job], indent=2) + "\n")
        self.mod.runner_info_path().write_text(
            json.dumps({"pid": 999999, "active_job_id": broader_job["id"], "active_branch": broader_job["branch"]}) + "\n"
        )

        queue = self.mod.load_queue()
        broader_stored = next(job for job in queue if job["id"] == broader_job["id"])
        narrower_stored = next(job for job in queue if job["id"] == narrower_job["id"])

        self.assertEqual(broader_stored["status"], "completed")
        self.assertEqual(broader_stored["overall"], "superseded")
        self.assertEqual(broader_stored["superseded_by"], narrower_job["id"])
        self.assertEqual(broader_stored["superseded_reason"], "narrower_scope_queued")
        self.assertEqual(narrower_stored["status"], "pending")

    def test_write_runner_info_is_safe_under_concurrent_updates(self):
        barrier = threading.Barrier(2)
        errors = []
        original_replace = self.mod.Path.replace

        def synchronized_replace(path, target):
            if path.name.startswith(".runner.json.") and path.suffix == ".tmp":
                barrier.wait(timeout=5)
            return original_replace(path, target)

        self.mod.Path.replace = synchronized_replace
        try:
            def worker(index):
                try:
                    self.mod.write_runner_info(
                        {
                            "pid": os.getpid(),
                            "root": f"/tmp/pulp-{index}",
                            "active_job_id": f"job{index}",
                            "active_branch": f"feature/{index}",
                        }
                    )
                except Exception as exc:  # pragma: no cover - regression guard
                    errors.append(exc)

            threads = [threading.Thread(target=worker, args=(i,)) for i in (1, 2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
        finally:
            self.mod.Path.replace = original_replace

        self.assertEqual(errors, [])
        info = self.mod.read_runner_info()
        self.assertIn(info["active_job_id"], {"job1", "job2"})
        self.assertIn(info["active_branch"], {"feature/1", "feature/2"})

    def test_windows_validation_can_pass_generator_instance(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job123", "branch": "feature/arm", "sha": "a" * 40},
                cmake_generator="Visual Studio 17 2022",
                cmake_platform="ARM64",
                cmake_generator_instance="C:/Program Files/Microsoft Visual Studio/2022/Community",
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertEqual(captured["cmd"][:2], ["ssh", "win"])
        self.assertIn(
            "-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance",
            captured["input_text"],
        )
        self.assertIn(
            "$GeneratorInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Community'",
            captured["input_text"],
        )
        self.assertIn("$Platform = 'ARM64'", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job123.bundle'", captured["input_text"])
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/job123'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job123", captured["input_text"])

    def test_windows_validation_rejects_missing_repo_probe_payload(self):
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = lambda host, repo_path, remote_url=None, **kwargs: None
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job123n", "branch": "feature/null-probe", "sha": "a" * 40},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "error")
        self.assertIn("no structured payload", result["stderr_tail"])

    def test_windows_validation_passes_config_to_bundle_upload_probe(self):
        captured = {"config": None}
        config = json.loads(self.config_path.read_text())
        config["targets"]["windows"]["host"] = "desktop.example.com"

        def fake_run_logged_command(cmd, **kwargs):
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        def fake_sync_bundle(host, job, report_progress=None, config=None):
            captured["config"] = config
            return (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "desktop.example.com",
                "C:\\Pulp",
                {"id": "job123b", "branch": "feature/arm", "sha": "b" * 40},
                config=config,
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIs(captured["config"], config)

    def test_windows_single_target_rerun_enables_prepared_reuse(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job127", "branch": "feature/rerun", "sha": "f" * 40, "targets": ["windows"]},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("$PreparedRoot = Join-Path $CiRoot 'prepared\\windows'", captured["input_text"])
        self.assertIn("$ReusePrepared = $true", captured["input_text"])
        self.assertIn("__PULP_PREPARED__:reused", captured["input_text"])
        self.assertIn("function Remove-DirectoryTreeRobust", captured["input_text"])
        self.assertIn("""cmd.exe /d /c ('rmdir /s /q "{0}"' -f $Path) | Out-Null""", captured["input_text"])
        self.assertIn("""$LongPath = if ($Path.StartsWith('\\\\?\\')) { $Path } else { '\\\\?\\' + $Path }""", captured["input_text"])
        self.assertIn("if (-not (Test-CommitRef $Sha)) {\n        try {\n            Invoke-Native git @('fetch', 'origin')", captured["input_text"])

    def test_windows_smoke_validation_installs_sdk_and_skips_ctest(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "__PULP_VALIDATION__:smoke\n__PULP_TEST_POLICY__:skip\nok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job126", "branch": "feature/smoke", "sha": "e" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("$ValidationMode = 'smoke'", captured["input_text"])
        self.assertIn('Write-Host "__PULP_VALIDATION__:$ValidationMode"', captured["input_text"])
        self.assertIn('Write-Host "__PULP_TEST_POLICY__:skip"', captured["input_text"])
        self.assertIn("-DPULP_BUILD_TESTS=OFF", captured["input_text"])
        self.assertIn("'--install'", captured["input_text"])
        self.assertIn("__PULP_PHASE__:smoke", captured["input_text"])
        self.assertIn("$smokeConfigureArgs = @('-S', $Smoke, '-B', (Join-Path $Smoke 'build'))", captured["input_text"])
        self.assertIn("$smokeConfigureArgs += @('-G', $Generator)", captured["input_text"])
        self.assertIn("$smokeConfigureArgs += @('-A', $Platform)", captured["input_text"])
        self.assertIn('$smokeConfigureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")', captured["input_text"])

    def test_windows_smoke_validation_fails_when_smoke_contract_markers_are_missing(self):
        def fake_run_logged_command(cmd, **kwargs):
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job126s", "branch": "feature/smoke", "sha": "f" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])

    def test_windows_validation_auto_detects_platform_and_vs_instance(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (
                "ARM64",
                "C:/Program Files/Microsoft Visual Studio/2022/Community",
            )
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job123", "branch": "feature/arm", "sha": "b" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("$Platform = 'ARM64'", captured["input_text"])
        self.assertIn(
            "$GeneratorInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Community'",
            captured["input_text"],
        )
        self.assertIn("$BundleName = 'pulp-ci-job123.bundle'", captured["input_text"])

    def test_windows_validation_recovers_abandoned_host_mutex(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job124", "branch": "feature/mutex", "sha": "c" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("function Wait-HostMutex", captured["input_text"])
        self.assertIn("AbandonedMutexException", captured["input_text"])
        self.assertIn("Recovered abandoned host validation lock: $MutexName", captured["input_text"])
        self.assertIn('Write-Host "__PULP_VALIDATOR_PID__:$PID"', captured["input_text"])
        self.assertIn('Write-Host "__PULP_VALIDATOR_STARTED__:$ValidatorStartedAt"', captured["input_text"])

    def test_reclaim_stale_remote_validators_cleans_targeted_windows_pid(self):
        job, _created = self.mod.enqueue_job(
            "feature/stale",
            "d" * 40,
            "normal",
            ["windows"],
            "run",
            "full",
        )

        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            queue = self.mod.load_queue_unlocked()
            stored = self.mod.find_job_unlocked(queue, job["id"])
            self.assertIsNotNone(stored)
            stored["status"] = "running"
            stored["runner"] = {"pid": 999999, "root": "/dead-runner"}
            stored["active_targets"] = {
                "windows": {
                    "status": "running",
                    "host": "win",
                    "validator_pid": 4321,
                    "validator_started_at": "2026-04-02T04:00:00+00:00",
                    "phase": "waiting-lock",
                }
            }
            self.mod.save_queue_unlocked(queue)

        with mock.patch.object(
            self.mod,
            "cleanup_stale_windows_validator",
            return_value={"found": True, "matched": True, "killed": True, "pid": 4321},
        ) as cleanup:
            reclaimed = self.mod.reclaim_stale_remote_validators({})

        self.assertEqual(reclaimed, 1)
        cleanup.assert_called_once_with("win", 4321, "2026-04-02T04:00:00+00:00")

        refreshed = self.mod.load_job(job["id"])
        self.assertIsNotNone(refreshed)
        state = refreshed["active_targets"]["windows"]
        self.assertEqual(state["cleanup_status"], "killed")
        self.assertIn("cleanup_completed_at", state)
        self.assertNotIn("validator_pid", state)
        self.assertNotIn("validator_started_at", state)

    def test_windows_validation_checks_commit_refs_quietly(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job125", "branch": "feature/commit-probe", "sha": "d" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("git rev-parse --verify --quiet", captured["input_text"])

    def test_windows_validation_fetches_branch_with_explicit_refspec(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job126", "branch": "feature/refspec", "sha": "e" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("refs/heads/$Branch`:refs/remotes/origin/$Branch", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job126.bundle'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job126", captured["input_text"])

    def test_validate_build_preserves_original_args_for_lock_reexec(self):
        text = VALIDATE_BUILD_PATH.read_text()
        self.assertIn('ORIGINAL_ARGS=("$@")', text)
        self.assertIn('if ((${#ORIGINAL_ARGS[@]})); then', text)
        self.assertIn('acquire_validation_lock "${ORIGINAL_ARGS[@]}"', text)
        self.assertIn('else\n    acquire_validation_lock\nfi', text)

    def test_validate_build_no_args_survives_strict_empty_array(self):
        env = os.environ.copy()
        env["PULP_VALIDATE_NO_LOCK"] = "1"
        env["PULP_EXPECT_SMOKE"] = "1"

        result = subprocess.run(
            ["bash", str(VALIDATE_BUILD_PATH)],
            cwd=VALIDATE_BUILD_PATH.parent,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("Smoke validation contract violated", result.stderr)
        self.assertNotIn("unbound variable", result.stderr)

    def test_validate_build_uses_release_sdk_for_install_smoke(self):
        text = VALIDATE_BUILD_PATH.read_text()
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", text)
        self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", text)

        ps1 = VALIDATE_BUILD_PATH.with_suffix(".ps1").read_text()
        self.assertIn('"-DCMAKE_BUILD_TYPE=Release"', ps1)
        self.assertIn("cmake --build $BuildDir --config Release", ps1)
        self.assertIn("cmake --install $BuildDir --prefix $InstallDir --config Release", ps1)
        self.assertIn("ctest --test-dir $BuildDir --output-on-failure -C Release", ps1)

    def test_save_result_updates_evidence_index_with_last_good_target_results(self):
        result_path_one = self.mod.save_result(
            {
                "job_id": "job111",
                "branch": "feature/evidence",
                "sha": "1" * 40,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac", "ubuntu"],
                "queued_at": "2026-04-01T00:00:00+00:00",
                "completed_at": "2026-04-01T00:10:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 10.0},
                    {"target": "ubuntu", "status": "fail", "duration_secs": 20.0},
                ],
                "overall": "fail",
            }
        )
        self.assertTrue(result_path_one.exists())

        self.mod.save_result(
            {
                "job_id": "job112",
                "branch": "feature/evidence",
                "sha": "1" * 40,
                "priority": "normal",
                "validation": "full",
                "targets": ["ubuntu"],
                "queued_at": "2026-04-01T00:11:00+00:00",
                "completed_at": "2026-04-01T00:20:00+00:00",
                "results": [
                    {"target": "ubuntu", "status": "pass", "duration_secs": 12.0},
                ],
                "overall": "pass",
            }
        )

        index = self.mod.load_evidence_index()
        self.assertIn(
            self.mod.evidence_entry_key("feature/evidence", "1" * 40, "mac", "full"),
            index["entries"],
        )
        self.assertIn(
            self.mod.evidence_entry_key("feature/evidence", "1" * 40, "ubuntu", "full"),
            index["entries"],
        )
        self.assertEqual(
            index["entries"][
                self.mod.evidence_entry_key("feature/evidence", "1" * 40, "ubuntu", "full")
            ]["job_id"],
            "job112",
        )

    def test_branch_scoped_evidence_survives_same_sha_on_another_branch(self):
        shared_sha = "4" * 40
        self.mod.save_result(
            {
                "job_id": "job401",
                "branch": "feature/alpha",
                "sha": shared_sha,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac"],
                "queued_at": "2026-04-01T03:00:00+00:00",
                "completed_at": "2026-04-01T03:10:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 8.0},
                ],
                "overall": "pass",
            }
        )
        self.mod.save_result(
            {
                "job_id": "job402",
                "branch": "main",
                "sha": shared_sha,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac"],
                "queued_at": "2026-04-01T03:11:00+00:00",
                "completed_at": "2026-04-01T03:20:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 7.5},
                ],
                "overall": "pass",
            }
        )

        feature_groups = self.mod.collect_evidence_groups(branch="feature/alpha")
        self.assertEqual(len(feature_groups["full"]), 1)
        self.assertEqual(feature_groups["full"][0]["sha"], shared_sha)
        self.assertEqual(feature_groups["full"][0]["branch"], "feature/alpha")
        self.assertIn("mac", feature_groups["full"][0]["targets"])

    def test_cmd_evidence_prints_grouped_branch_summary(self):
        self.mod.save_result(
            {
                "job_id": "job201",
                "branch": "feature/evidence",
                "sha": "2" * 40,
                "priority": "normal",
                "validation": "smoke",
                "targets": ["mac", "windows"],
                "queued_at": "2026-04-01T01:00:00+00:00",
                "completed_at": "2026-04-01T01:10:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 9.0},
                    {"target": "windows", "status": "pass", "duration_secs": 15.0},
                ],
                "overall": "pass",
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_evidence(
                SimpleNamespace(branch="feature/evidence", sha=None, limit=5)
            )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Evidence for branch `feature/evidence`:", output)
        self.assertIn("smoke:", output)
        self.assertIn("mac=pass, windows=pass", output)
        self.assertIn("222222222222", output)

    def test_cmd_status_includes_current_branch_evidence_summary(self):
        self.mod.save_result(
            {
                "job_id": "job301",
                "branch": "feature/status-evidence",
                "sha": "3" * 40,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac", "ubuntu", "windows"],
                "queued_at": "2026-04-01T02:00:00+00:00",
                "completed_at": "2026-04-01T02:30:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 10.0},
                    {"target": "ubuntu", "status": "pass", "duration_secs": 12.0},
                    {"target": "windows", "status": "pass", "duration_secs": 14.0},
                ],
                "overall": "pass",
            }
        )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/status-evidence"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Evidence (feature/status-evidence):", output)
        self.assertIn("333333333333", output)
        self.assertIn("mac=pass, ubuntu=pass, windows=pass", output)

    def test_cmd_status_shows_heartbeat_idle_and_liveness(self):
        job, _created = self.mod.enqueue_job(
            "feature/observability",
            "4" * 40,
            "normal",
            ["windows"],
            "run",
            "full",
        )
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            queue = self.mod.load_queue_unlocked()
            stored = self.mod.find_job_unlocked(queue, job["id"])
            self.assertIsNotNone(stored)
            stored["status"] = "running"
            stored["started_at"] = "2026-04-02T05:00:00+00:00"
            stored["runner"] = {"pid": os.getpid(), "root": str(self.state_dir)}
            stored["active_targets"] = {
                "windows": {
                    "status": "running",
                    "phase": "build",
                    "last_output_at": "2026-04-02T05:00:10+00:00",
                    "last_heartbeat_at": "2026-04-02T05:01:10+00:00",
                    "quiet_for_secs": 60,
                    "liveness": "stuck",
                    "log_path": str(self.state_dir / "logs" / "job.log"),
                }
            }
            self.mod.save_queue_unlocked(queue)
            self.mod.write_runner_info(
                {
                    "pid": os.getpid(),
                    "root": str(self.state_dir),
                    "active_job_id": job["id"],
                    "active_branch": job["branch"],
                    "active_targets": stored["active_targets"],
                }
            )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/observability"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("heartbeat=2026-04-02T05:01:10+00:00", output)
        self.assertIn("idle=60s", output)
        self.assertIn("liveness=stuck", output)

    def test_build_submission_metadata_adds_default_provenance(self):
        config = self.mod.load_config()
        metadata = self.mod.build_submission_metadata(
            config,
            "feature/provenance",
            "a" * 40,
            ["mac"],
            "normal",
            "full",
            allow_root_mismatch=True,
            allow_unreachable_targets=False,
        )

        self.assertEqual(metadata["provenance"]["execution_kind"], "direct")
        self.assertEqual(metadata["provenance"]["control_plane"], "pulp-ci-local")
        self.assertEqual(metadata["provenance"]["direct_backend"], "local-ci")
        self.assertEqual(metadata["provenance"]["hosted_orchestrator"], "")

    def test_evidence_record_carries_provenance(self):
        result = {
            "job_id": "job123",
            "branch": "feature/evidence",
            "sha": "c" * 40,
            "validation": "full",
            "completed_at": "2026-04-04T12:00:00+00:00",
            "provenance": {
                "execution_kind": "hosted",
                "control_plane": "pulp-ci-local",
                "direct_backend": "",
                "hosted_orchestrator": "github-actions",
                "runner_provider": "namespace",
                "runner_selector": "mac-arm64",
                "run_id": "12345",
                "run_url": "https://example.test/runs/12345",
            },
        }
        item = {"target": "mac", "status": "pass", "duration_secs": 12}

        record = self.mod.evidence_record_from_result(result, item, self.state_dir / "result.json")
        self.assertEqual(record["provenance"]["hosted_orchestrator"], "github-actions")
        self.assertEqual(record["provenance"]["runner_provider"], "namespace")
        self.assertEqual(record["provenance"]["runner_selector"], "mac-arm64")

    def test_cmd_cloud_run_rejects_unsupported_provider(self):
        original_gh_available = self.cloud.gh_available
        self.cloud.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="validate",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("does not support provider", output)

    def test_cmd_cloud_run_build_namespace_dispatches_selector_fields(self):
        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_now_iso = self.cloud.now_iso
        original_repo_variables = self.cloud.gh_repo_variables

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        self.cloud.gh_repo_variables = lambda repository: {}
        dispatched = {}
        self.cloud.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.now_iso = original_now_iso
            self.cloud.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.cloud.list_cloud_records()
        self.assertEqual(records[0]["dispatch_fields"]["linux_runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["dispatch_fields"]["windows_runner_selector_json"], "\"namespace-profile-default\"")

    def test_cmd_cloud_run_build_namespace_uses_repo_variable_selector_defaults(self):
        config = json.loads(self.config_path.read_text())
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["linux_runner_selector_json"]
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["windows_runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_now_iso = self.cloud.now_iso
        original_repo_variables = self.cloud.gh_repo_variables

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        self.cloud.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux-repo\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows-repo\"",
        }
        dispatched = {}
        self.cloud.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.now_iso = original_now_iso
            self.cloud.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-linux-repo\"",
                "windows_runner_selector_json": "\"namespace-profile-windows-repo\"",
            },
        )

    def test_cmd_cloud_run_build_namespace_includes_optional_macos_selector_when_present(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["workflows"]["build"]["providers"]["namespace"][
            "macos_runner_selector_json"
        ] = "\"namespace-profile-macos\""
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_now_iso = self.cloud.now_iso

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.cloud.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-macos\"",
        )

    def test_cmd_cloud_run_build_cli_override_adds_one_off_macos_selector(self):
        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_now_iso = self.cloud.now_iso

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.cloud.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json="\"namespace-profile-big-apple\"",
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-big-apple\"",
        )

    def test_cmd_cloud_run_rejects_build_leg_override_for_docs_check(self):
        original_gh_available = self.cloud.gh_available
        self.cloud.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json="\"namespace-profile-big-apple\"",
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("--macos-runner-selector-json is not supported", output)

    def test_cmd_cloud_run_dispatches_waits_and_persists_record(self):
        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_view = self.cloud.gh_run_view
        original_sleep = self.mod.time.sleep
        original_now_iso = self.cloud.now_iso

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.cloud.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
            "databaseId": 98765,
            "headBranch": ref,
            "headSha": "e" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:05+00:00",
            "workflowName": "Docs Consistency",
            "match_ambiguous": False,
        }
        self.cloud.gh_run_view = lambda repository, run_id: {
            "databaseId": run_id,
            "status": "completed",
            "conclusion": "success",
            "url": "https://example.test/runs/98765",
            "headSha": "e" * 40,
            "headBranch": "feature/cloud",
            "workflowName": "Docs Consistency",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:10+00:00",
            "jobs": [
                {
                    "name": "Validate docs consistency",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:06+00:00",
                    "completedAt": "2026-04-04T12:00:10+00:00",
                }
            ],
        }
        self.mod.time.sleep = lambda _: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=True,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.gh_run_view = original_view
            self.mod.time.sleep = original_sleep
            self.cloud.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(dispatched["workflow_file"], "docs-check.yml")
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.cloud.list_cloud_records()
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["run_id"], 98765)
        self.assertEqual(records[0]["provider_resolved"], "namespace")
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["conclusion"], "success")

    def test_cmd_cloud_run_explicit_runner_selector_overrides_config_default(self):
        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_now_iso = self.cloud.now_iso

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.cloud.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json="\"namespace-profile-big-apple\"",
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-big-apple\"",
            },
        )
        records = self.cloud.list_cloud_records()
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-big-apple\"")

    def test_cmd_cloud_status_shows_runner_selector(self):
        self.cloud.save_cloud_record(
            {
                "dispatch_id": "sel123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "started_at": "2026-04-04T12:00:06+00:00",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "queue_delay_secs": 1,
                "duration_secs": 24,
                "usage_summary": {
                    "instances_count": 2,
                    "provider_runtime_secs": 75,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 2,
                            "duration_secs": 75,
                        }
                    ],
                },
                "cost_summary": {
                    "status": "unavailable",
                    "reason": "Namespace CLI does not expose billing totals; provider runtime is shown instead.",
                },
                "jobs": [
                    {
                        "name": "Validate docs consistency",
                        "status": "completed",
                        "conclusion": "success",
                        "started_at": "2026-04-04T12:00:09+00:00",
                        "completed_at": "2026-04-04T12:00:30+00:00",
                    }
                ],
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.cloud.cmd_cloud_status(
                SimpleNamespace(identifier="latest", refresh=False, limit=5)
            )

        self.assertEqual(exit_code, 0)
        self.assertIn("runner selector: namespace-profile-default", buf.getvalue())
        self.assertIn("queue delay: 1s", buf.getvalue())
        self.assertIn("elapsed: 24s", buf.getvalue())
        self.assertIn("provider usage: 2 Namespace instance(s) runtime=1m15s", buf.getvalue())
        self.assertIn("namespace-profile-default: linux/amd64 4 vCPU 8 GB x2 runtime=1m15s", buf.getvalue())
        self.assertIn("cost: unavailable", buf.getvalue())
        self.assertIn("duration=21s", buf.getvalue())

    def test_cmd_cloud_defaults_reports_effective_providers_and_sources(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["provider"] = "namespace"
        del config["github_actions"]["workflows"]["docs-check"]["providers"]["namespace"]["runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.cloud.gh_available
        original_repo_variables = self.cloud.gh_repo_variables
        self.cloud.gh_available = lambda: True
        self.cloud.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-docs\"",
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows\"",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.gh_repo_variables = original_repo_variables

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("configured default provider: namespace", output)
        self.assertIn("billing estimates: USD period-day=1 (estimated; verify provider pricing)", output)
        self.assertIn("provider billing truth: disabled (opt-in; off by default)", output)
        self.assertIn("build: Build and Test (build.yml)", output)
        self.assertIn("linux_runner_selector_json: namespace-profile-default", output)
        self.assertIn("docs-check: Docs Consistency (docs-check.yml)", output)
        self.assertIn("runner_selector_json: namespace-profile-docs (repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON)", output)
        self.assertIn("validate: Plugin Validation (validate.yml)", output)
        self.assertIn("default provider: github-hosted (workflow fallback", output)

    def test_cmd_cloud_defaults_handles_invalid_timing_config(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["wait_poll_secs"] = "not-an-int"
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.cloud.gh_available
        self.cloud.gh_available = lambda: False
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.cloud.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("repository: danielraffel/pulp", output)
        self.assertIn("note: github_actions.defaults.wait_poll_secs must be an integer.", output)
        self.assertIn("configured default workflow: build", output)
        self.assertIn("configured default provider: github-hosted", output)

    def test_estimate_cloud_record_cost_uses_namespace_profile_rate(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }

        summary = self.cloud.estimate_cloud_record_cost(
            {
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 7200,
                        }
                    ]
                },
            },
            config,
        )

        self.assertEqual(summary["status"], "estimated")
        self.assertEqual(summary["currency"], "USD")
        self.assertAlmostEqual(summary["estimated_total"], 1.0)
        self.assertEqual(summary["reason"], "estimated; verify provider pricing")

    def test_fetch_github_repo_actions_billing_summary_sums_repo_usage(self):
        config = {
            "telemetry": {
                "billing": {
                    "enable_provider_reported_totals": True,
                }
            }
        }

        original_gh_available = self.cloud.gh_available
        original_gh_api_json = self.cloud.gh_api_json
        original_billing_window = self.cloud.billing_period_window
        self.cloud.gh_available = lambda: True
        self.cloud.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 3, 15, tzinfo=timezone.utc),
            datetime(2026, 4, 15, tzinfo=timezone.utc),
        )

        def fake_gh_api_json(path, fields=None):
            if path == "/repos/danielraffel/pulp":
                return ({"owner": {"login": "danielraffel", "type": "User"}}, "")
            if path == "/users/danielraffel/settings/billing/usage":
                if fields == {"year": 2026, "month": 3}:
                    return (
                        {
                            "usageItems": [
                                {
                                    "date": "2026-03-14",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 1.0,
                                },
                                {
                                    "date": "2026-03-15",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 2.0,
                                },
                            ]
                        },
                        "",
                    )
                if fields == {"year": 2026, "month": 4}:
                    return (
                        {
                            "usageItems": [
                                {
                                    "date": "2026-04-01",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 3.5,
                                },
                                {
                                    "date": "2026-04-02",
                                    "product": "Packages",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 9.0,
                                },
                                {
                                    "date": "2026-04-03",
                                    "product": "Actions",
                                    "repositoryName": "other/repo",
                                    "netAmount": 7.0,
                                },
                            ]
                        },
                        "",
                    )
            return (None, "unexpected call")

        self.cloud.gh_api_json = fake_gh_api_json
        try:
            summary = self.cloud.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.gh_api_json = original_gh_api_json
            self.cloud.billing_period_window = original_billing_window

        self.assertEqual(summary["status"], "actual")
        self.assertEqual(summary["currency"], "USD")
        self.assertAlmostEqual(summary["actual_total"], 5.5)
        self.assertEqual(summary["matched_items"], 2)
        self.assertEqual(summary["reason"], "actual when available")

    def test_cmd_cloud_history_shows_estimated_cost_and_period_total(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.cloud.save_cloud_record(
            {
                "dispatch_id": "hist123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "duration_secs": 24,
                "completed_at": "2026-04-04T12:00:30+00:00",
                "usage_summary": {
                    "instances_count": 1,
                    "provider_runtime_secs": 3600,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 1,
                            "duration_secs": 3600,
                        }
                    ],
                },
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 3600,
                        }
                    ]
                },
            }
        )

        original_billing_period_window = self.cloud.billing_period_window
        self.cloud.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 4, 1, tzinfo=timezone.utc),
            datetime(2026, 5, 1, tzinfo=timezone.utc),
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_history(
                    SimpleNamespace(workflow=None, provider=None, limit=10)
                )
        finally:
            self.cloud.billing_period_window = original_billing_period_window

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("cost=est $0.50", output)
        self.assertIn("period cost: est $0.50 over 1 run(s); estimated; verify provider pricing", output)

    def test_cmd_cloud_history_shows_provider_reported_github_billing_when_enabled(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "enable_provider_reported_totals": True,
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.cloud.save_cloud_record(
            {
                "dispatch_id": "histgh123456",
                "workflow_key": "build",
                "workflow_name": "Build and Test",
                "workflow_file": "build.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "run_id": 12345,
                "duration_secs": 30,
                "completed_at": "2026-04-04T12:00:30+00:00",
            }
        )

        original_fetch = self.cloud.fetch_github_repo_actions_billing_summary
        self.cloud.fetch_github_repo_actions_billing_summary = lambda repository, cfg: {
            "status": "actual",
            "currency": "USD",
            "actual_total": 2.7,
            "reason": "actual when available",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_history(
                    SimpleNamespace(workflow=None, provider=None, limit=10)
                )
        finally:
            self.cloud.fetch_github_repo_actions_billing_summary = original_fetch

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("github repo billing: actual $2.70 current period (repo-wide)", output)

    def test_cmd_cloud_compare_reports_provider_medians(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "github_hosted_job_os_rates_per_minute": {
                    "linux": 0.01
                },
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.cloud.save_cloud_record(
            {
                "dispatch_id": "cmpns123456",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "duration_secs": 120,
                "queue_delay_secs": 5,
                "usage_summary": {
                    "provider_runtime_secs": 3600,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 1,
                            "duration_secs": 3600,
                        }
                    ],
                },
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 3600,
                        }
                    ]
                },
            }
        )
        self.cloud.save_cloud_record(
            {
                "dispatch_id": "cmpgh123456",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:10:30+00:00",
                "duration_secs": 180,
                "queue_delay_secs": 15,
                "jobs": [
                    {
                        "name": "Linux (x64) [github-hosted]",
                        "started_at": "2026-04-04T12:07:30+00:00",
                        "completed_at": "2026-04-04T12:10:30+00:00",
                    }
                ],
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.cloud.cmd_cloud_compare(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn(
            "github-hosted: runs=1 success=1/1 median_elapsed=3m00s median_queue=15s median_cost=est $0.03 latest_success=2026-04-04T12:10:30+00:00",
            output,
        )
        self.assertIn(
            "namespace: runs=1 success=1/1 median_elapsed=2m00s median_queue=5s median_provider_time=1h00m00s median_cost=est $0.50 latest_success=2026-04-04T12:00:30+00:00",
            output,
        )
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_recommend_prefers_fastest_observed_provider(self):
        self.cloud.save_cloud_record(
            {
                "dispatch_id": "recns123456",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "duration_secs": 120,
            }
        )
        self.cloud.save_cloud_record(
            {
                "dispatch_id": "recgh123456",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:10:30+00:00",
                "duration_secs": 180,
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.cloud.cmd_cloud_recommend(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Recommended provider for build: namespace (fastest observed median)", output)
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_run_wait_fails_when_refresh_cannot_fetch_github_state(self):
        original_gh_available = self.cloud.gh_available
        original_resolve_repo = self.cloud.resolve_github_repository
        original_current_login = self.cloud.gh_current_login
        original_dispatch = self.cloud.gh_workflow_dispatch
        original_find = self.cloud.gh_find_dispatched_run
        original_view = self.cloud.gh_run_view
        original_sleep = self.mod.time.sleep
        original_now_iso = self.cloud.now_iso

        self.cloud.gh_available = lambda: True
        self.cloud.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.cloud.gh_current_login = lambda: "danielraffel"
        self.cloud.gh_workflow_dispatch = lambda repository, workflow_file, ref, fields: None
        self.cloud.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
            "databaseId": 98765,
            "workflowName": "Docs Consistency",
            "headBranch": "feature/cloud",
            "headSha": "a" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:06+00:00",
            "jobs": [],
        }
        self.cloud.gh_run_view = lambda repository, run_id: None
        self.mod.time.sleep = lambda _: None
        self.cloud.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=True,
                    )
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.resolve_github_repository = original_resolve_repo
            self.cloud.gh_current_login = original_current_login
            self.cloud.gh_workflow_dispatch = original_dispatch
            self.cloud.gh_find_dispatched_run = original_find
            self.cloud.gh_run_view = original_view
            self.mod.time.sleep = original_sleep
            self.cloud.now_iso = original_now_iso

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("Error: Failed to refresh GitHub run 98765 from danielraffel/pulp.", output)

    def test_cmd_status_includes_recent_cloud_summary(self):
        self.cloud.save_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Cloud defaults: workflow=build provider=github-hosted", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)
        self.assertIn("docs-check", output)
        self.assertIn("gha#98765", output)

    def test_cmd_cloud_status_refresh_uses_record_repository(self):
        self.cloud.save_cloud_record(
            {
                "dispatch_id": "repo123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "other-owner/other-repo",
                "requested_ref": "feature/cloud",
                "provider_requested": "github-hosted",
                "status": "in_progress",
                "run_id": 77777,
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        original_gh_available = self.cloud.gh_available
        original_view = self.cloud.gh_run_view
        seen = {}
        self.cloud.gh_available = lambda: True
        self.cloud.gh_run_view = lambda repository, run_id: (
            seen.update({"repository": repository, "run_id": run_id}) or {
                "databaseId": 77777,
                "workflowName": "Docs Consistency",
                "headBranch": "feature/cloud",
                "headSha": "a" * 40,
                "status": "completed",
                "conclusion": "success",
                "url": "https://example.test/runs/77777",
                "createdAt": "2026-04-04T12:00:05+00:00",
                "updatedAt": "2026-04-04T12:00:30+00:00",
                "jobs": [],
            }
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.cloud.cmd_cloud_status(
                    SimpleNamespace(identifier="latest", refresh=True, limit=5)
                )
        finally:
            self.cloud.gh_available = original_gh_available
            self.cloud.gh_run_view = original_view

        self.assertEqual(exit_code, 0)
        self.assertEqual(seen["repository"], "other-owner/other-repo")
        self.assertEqual(seen["run_id"], 77777)

    def test_cmd_status_handles_invalid_cloud_defaults_config(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["wait_poll_secs"] = "broken"
        self.config_path.write_text(json.dumps(config) + "\n")

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Cloud defaults: workflow=build provider=github-hosted", output)
        self.assertIn("note: github_actions.defaults.wait_poll_secs must be an integer.", output)

    def test_cmd_status_period_cost_uses_full_cloud_history(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        for index in range(6):
            self.cloud.save_cloud_record(
                {
                    "dispatch_id": f"hist{index:02d}abcdef",
                    "workflow_key": "build",
                    "provider_requested": "namespace",
                    "provider_resolved": "namespace",
                    "status": "completed",
                    "conclusion": "success",
                    "completed_at": f"2026-04-04T12:0{index}:30+00:00",
                    "duration_secs": 24,
                    "provider_metadata": {
                        "namespace_instances": [
                            {
                                "profile_tag": "namespace-profile-default",
                                "os": "linux",
                                "arch": "amd64",
                                "virtual_cpu": 4,
                                "memory_megabytes": 8192,
                                "duration_secs": 3600,
                            }
                        ]
                    },
                }
            )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        original_billing_period_window = self.cloud.billing_period_window
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        self.cloud.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 4, 1, tzinfo=timezone.utc),
            datetime(2026, 5, 1, tzinfo=timezone.utc),
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable
            self.cloud.billing_period_window = original_billing_period_window

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("period cost: est $3.00 over 6 run(s); estimated; verify provider pricing", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)

    def test_finalize_job_prunes_completed_job_bundle_but_keeps_retained_logs_and_results(self):
        running_job = {
            "id": "job123456789",
            "branch": "feature/job",
            "sha": "a" * 40,
            "priority": "normal",
            "targets": ["mac"],
            "queued_at": "2026-04-04T12:00:00+00:00",
            "status": "running",
            "fingerprint": "job",
            "mode": "run",
            "validation": "full",
        }
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked([running_job])

        bundle_path = self.state_dir / "bundles" / "job123456789.bundle"
        bundle_path.parent.mkdir(parents=True, exist_ok=True)
        bundle_path.write_bytes(b"bundle")

        log_dir = self.state_dir / "logs" / "job123456789"
        log_dir.mkdir(parents=True, exist_ok=True)
        (log_dir / "mac.log").write_text("keep")

        result_path = self.state_dir / "results" / "20260404-120000-job123456789-feature-job.json"
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text("{}\n")

        self.mod.finalize_job(
            "job123456789",
            {"overall": "pass"},
            result_path,
        )

        self.assertFalse(bundle_path.exists())
        self.assertTrue(log_dir.exists())
        self.assertTrue(result_path.exists())

if __name__ == "__main__":
    unittest.main()
