#!/usr/bin/env python3

import io
import importlib.util
import json
import os
import tempfile
import threading
import unittest
from unittest import mock
from contextlib import redirect_stdout
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


class LocalCiTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
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

        result = self.mod.load_result(Path(older_stored["result_file"]))
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
        result = self.mod.load_result(Path(stored["result_file"]))
        self.assertEqual(result["overall"], "canceled")
        self.assertEqual(result["canceled_reason"], "operator_canceled")

    def test_resolve_targets_uses_defaults_and_rejects_disabled_targets(self):
        config = self.mod.load_config()
        self.assertEqual(self.mod.resolve_targets(config, None), ["mac"])

        with self.assertRaises(ValueError):
            self.mod.resolve_targets(config, ["windows"])

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

    def test_summarize_active_targets_uses_requested_order(self):
        summary = self.mod.summarize_active_targets(
            {
                "ubuntu": {"status": "running"},
                "windows": {"status": "pending"},
                "mac": {"status": "pass"},
            },
            ["mac", "ubuntu"],
        )
        self.assertEqual(summary, "mac=pass, ubuntu=running, windows=pending")

    def test_update_runner_active_targets_tracks_live_state(self):
        self.mod.write_runner_info(
            {
                "pid": os.getpid(),
                "root": "/tmp/pulp",
                "active_job_id": "job123",
                "active_branch": "feature/live-state",
            }
        )

        self.mod.update_runner_active_targets(
            "job123",
            {
                "mac": {"status": "pass", "duration_secs": 12.3},
                "windows": {"status": "running"},
            },
        )

        info = self.mod.read_runner_info()
        self.assertEqual(info["active_targets"]["mac"]["status"], "pass")
        self.assertEqual(info["active_targets"]["windows"]["status"], "running")
        self.assertIn("updated_at", info)

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

    def test_update_job_active_targets_tracks_live_state(self):
        job = self.mod.make_job("feature/progress", "4" * 40, "normal", ["mac", "ubuntu"], "run", "full")
        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([job], indent=2) + "\n")

        self.mod.update_job_active_targets(
            job["id"],
            {
                "mac": {"status": "pass", "duration_secs": 12.3},
                "ubuntu": {"status": "running"},
            },
        )

        queue = self.mod.load_queue()
        self.assertEqual(queue[0]["active_targets"]["mac"]["status"], "pass")
        self.assertEqual(queue[0]["active_targets"]["ubuntu"]["status"], "running")
        self.assertIn("last_progress_at", queue[0])

    def test_windows_ssh_powershell_command_uses_stdin_eval_wrapper(self):
        cmd = self.mod.windows_ssh_powershell_command("win2")
        self.assertEqual(
            cmd,
            [
                "ssh",
                "win2",
                "powershell",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
            ],
        )

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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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

        self.assertEqual(result["status"], "pass")
        self.assertIn("$PreparedRoot = Join-Path $CiRoot 'prepared\\windows'", captured["input_text"])
        self.assertIn("$ReusePrepared = $true", captured["input_text"])
        self.assertIn("__PULP_PREPARED__:reused", captured["input_text"])

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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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

        self.assertEqual(result["status"], "pass")
        self.assertIn("$ValidationMode = 'smoke'", captured["input_text"])
        self.assertIn("-DPULP_BUILD_TESTS=OFF", captured["input_text"])
        self.assertIn("'--install'", captured["input_text"])
        self.assertIn("__PULP_PHASE__:smoke", captured["input_text"])
        self.assertIn("$smokeConfigureArgs = @('-S', $Smoke, '-B', (Join-Path $Smoke 'build'))", captured["input_text"])
        self.assertIn("$smokeConfigureArgs += @('-G', $Generator)", captured["input_text"])
        self.assertIn("$smokeConfigureArgs += @('-A', $Platform)", captured["input_text"])
        self.assertIn('$smokeConfigureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")', captured["input_text"])

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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (
                "ARM64",
                "C:/Program Files/Microsoft Visual Studio/2022/Community",
            )
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
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

        self.assertEqual(result["status"], "pass")
        self.assertIn("refs/heads/$Branch`:refs/remotes/origin/$Branch", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job126.bundle'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job126", captured["input_text"])

    def test_sync_job_bundle_to_ssh_host_uses_scp_and_keeps_local_bundle(self):
        bundle_path = self.state_dir / "bundles" / "job777.bundle"
        captured = {}

        def fake_create_job_bundle(job):
            bundle_path.parent.mkdir(parents=True, exist_ok=True)
            bundle_path.write_text("bundle")
            return bundle_path

        def fake_run(cmd, **kwargs):
            captured["cmd"] = cmd
            captured["kwargs"] = kwargs
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        original_create = self.mod.create_job_bundle
        original_run = self.mod.subprocess.run
        self.mod.create_job_bundle = fake_create_job_bundle
        self.mod.subprocess.run = fake_run
        try:
            remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
                "win",
                {"id": "job777", "sha": "f" * 40},
            )
        finally:
            self.mod.create_job_bundle = original_create
            self.mod.subprocess.run = original_run

        self.assertEqual(remote_name, "pulp-ci-job777.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job777")
        self.assertEqual(captured["cmd"], ["scp", str(bundle_path), "win:pulp-ci-job777.bundle"])
        self.assertTrue(bundle_path.exists())

    def test_create_job_bundle_reuses_existing_artifact_across_threads(self):
        job = {"id": "job-concurrent", "sha": "a" * 40}
        bundle_path = self.state_dir / "bundles" / "job-concurrent.bundle"
        create_calls = []
        original_run = self.mod.subprocess.run

        def fake_run(cmd, cwd=None, check=None, **kwargs):
            if cmd[:3] == ["git", "bundle", "create"]:
                create_calls.append(cmd)
                bundle_path.parent.mkdir(parents=True, exist_ok=True)
                bundle_path.write_text("bundle")
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        self.mod.subprocess.run = fake_run
        try:
            results = []

            def worker():
                results.append(self.mod.create_job_bundle(job))

            threads = [threading.Thread(target=worker) for _ in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
        finally:
            self.mod.subprocess.run = original_run

        self.assertEqual(len(create_calls), 1)
        self.assertEqual(results, [bundle_path, bundle_path])
        self.assertTrue(bundle_path.exists())

    def test_posix_validation_fetches_uploaded_bundle_first(self):
        captured = {}

        def fake_sync_bundle(host, job, report_progress=None):
            return ("pulp-ci-job888.bundle", "refs/pulp-ci-bundles/job888")

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_run_logged = self.mod.run_logged_command
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu",
                "/tmp/pulp",
                {"id": "job888", "branch": "feature/bundle", "sha": "1" * 40},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "pass")
        remote_cmd = captured["cmd"][-1]
        self.assertIn("bundle-sync", remote_cmd)
        self.assertIn('bundle="$HOME/$bundle_name"', remote_cmd)
        self.assertIn('prepared_root="$HOME/.local/state/pulp/local-ci/prepared/ubuntu/full"', remote_cmd)
        self.assertIn('PULP_VALIDATE_REUSE_PREPARED="$reuse_prepared"', remote_cmd)
        self.assertIn('script="$PWD/$script_name"', remote_cmd)
        self.assertIn('git fetch "$bundle" "$bundle_ref:refs/remotes/origin/$branch"', remote_cmd)
        self.assertIn('git show "$sha:validate-build.sh" > "$script"', remote_cmd)
        self.assertIn('bash "$script" --quiet --keep-worktree --ref "$sha"', remote_cmd)

    def test_posix_smoke_validation_runs_sha_pinned_script_with_smoke_flag(self):
        captured = {}

        def fake_sync_bundle(host, job, report_progress=None):
            return ("pulp-ci-job889.bundle", "refs/pulp-ci-bundles/job889")

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "__PULP_VALIDATION__:smoke\n__PULP_TEST_POLICY__:skip\nok\n",
                "duration_secs": 1.2,
            }

        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_run_logged = self.mod.run_logged_command
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu",
                "/tmp/pulp",
                {"id": "job889", "branch": "feature/smoke", "sha": "2" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "pass")
        remote_cmd = captured["cmd"][-1]
        self.assertIn('script_name=.pulp-ci-validate-job889.sh', remote_cmd)
        self.assertIn('prepared_root="$HOME/.local/state/pulp/local-ci/prepared/ubuntu/smoke"', remote_cmd)
        self.assertIn('git show "$sha:validate-build.sh" > "$script"', remote_cmd)
        self.assertIn(
            'PULP_EXPECT_SMOKE=1 bash "$script" --quiet --keep-worktree --ref "$sha" --smoke --no-tests',
            remote_cmd,
        )

    def test_posix_smoke_validation_fails_when_smoke_contract_markers_are_missing(self):
        def fake_sync_bundle(host, job, report_progress=None):
            return ("pulp-ci-job890.bundle", "refs/pulp-ci-bundles/job890")

        def fake_run_logged_command(cmd, **kwargs):
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_run_logged = self.mod.run_logged_command
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu",
                "/tmp/pulp",
                {"id": "job890", "branch": "feature/smoke", "sha": "3" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])

    def test_probe_windows_ssh_cmake_settings_parses_remote_json(self):
        class FakeCompleted:
            def __init__(self):
                self.returncode = 0
                self.stdout = '\n{"platform":"ARM64","generator_instance":"C:/Program Files/Microsoft Visual Studio/2022/Community"}\n'

        original_run = self.mod.subprocess.run
        self.mod.subprocess.run = lambda *args, **kwargs: FakeCompleted()
        try:
            platform, instance = self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Visual Studio 17 2022",
                "",
                "",
            )
        finally:
            self.mod.subprocess.run = original_run

        self.assertEqual(platform, "ARM64")
        self.assertEqual(instance, "C:/Program Files/Microsoft Visual Studio/2022/Community")

    def test_run_logged_command_starts_reader_before_writing_input(self):
        read_started = threading.Event()
        read_finished = threading.Event()
        writes = []

        class FakeStdout:
            def __iter__(self):
                read_started.set()
                yield "ready\n"
                read_finished.set()

            def close(self):
                read_finished.set()

        class FakeStdin:
            def write(self, text):
                if not read_started.wait(timeout=1):
                    raise TimeoutError("reader did not start before stdin write")
                writes.append(text)

            def close(self):
                writes.append("<closed>")

        class FakeProc:
            def __init__(self):
                self.stdin = FakeStdin()
                self.stdout = FakeStdout()

            def poll(self):
                return 0 if read_finished.is_set() else None

            def wait(self, timeout=None):
                self.poll()
                read_finished.wait(timeout=timeout)
                return 0

            def kill(self):
                read_finished.set()

        original_popen = self.mod.subprocess.Popen
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        try:
            result = self.mod.run_logged_command(["ssh", "win2"], input_text="payload", timeout=5)
        finally:
            self.mod.subprocess.Popen = original_popen

        self.assertFalse(result["timed_out"])
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["output"], "ready\n")
        self.assertEqual(writes, ["payload", "<closed>"])

    def test_parse_progress_marker_detects_phase_wait_and_smoke_contract(self):
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_PHASE__:build\n"),
            {"phase": "build"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_WAIT__:host-lock\n"),
            {"wait_reason": "host-lock"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATION__:smoke\n"),
            {"validation_mode": "smoke"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_TEST_POLICY__:skip\n"),
            {"test_policy": "skip"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_PREPARED__:reused\n"),
            {"prepared_state": "reused"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_PID__:4321\n"),
            {"validator_pid": 4321},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_STARTED__:2026-04-02T04:00:00+00:00\n"),
            {"validator_started_at": "2026-04-02T04:00:00+00:00"},
        )
        self.assertEqual(self.mod.parse_progress_marker("normal output\n"), {})

    def test_validate_build_preserves_original_args_for_lock_reexec(self):
        text = VALIDATE_BUILD_PATH.read_text()
        self.assertIn('ORIGINAL_ARGS=("$@")', text)
        self.assertIn('acquire_validation_lock "${ORIGINAL_ARGS[@]}"', text)

    def test_run_local_validation_uses_prepared_root_for_single_target_reruns(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_local_validation(
                {"id": "job501", "branch": "feature/local", "sha": "5" * 40, "targets": ["mac"]}
            )
        finally:
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "pass")
        cmd = captured["cmd"]
        self.assertEqual(cmd[0], "env")
        self.assertIn("PULP_VALIDATE_REUSE_PREPARED=1", cmd)
        self.assertTrue(
            any(arg.startswith("PULP_VALIDATE_ROOT_OVERRIDE=") for arg in cmd),
            msg=f"missing prepared root override in {cmd}",
        )
        self.assertIn("--keep-worktree", cmd)

    def test_run_logged_command_keeps_progress_markers_in_output_and_reports_them(self):
        log_path = self.state_dir / "marker.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        seen = {}

        def report_progress(**fields):
            seen.update(fields)

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                (
                    "print('__PULP_VALIDATION__:smoke');"
                    "print('__PULP_TEST_POLICY__:skip');"
                    "print('__PULP_PHASE__:build');"
                    "print('done')"
                ),
            ],
            log_path=log_path,
            report_progress=report_progress,
        )

        self.assertEqual(result["returncode"], 0)
        self.assertIn("__PULP_VALIDATION__:smoke", result["output"])
        self.assertIn("__PULP_TEST_POLICY__:skip", result["output"])
        self.assertIn("__PULP_PHASE__:build", result["output"])
        self.assertIn("done", result["output"])
        logged = log_path.read_text()
        self.assertIn("__PULP_VALIDATION__:smoke", logged)
        self.assertIn("__PULP_TEST_POLICY__:skip", logged)
        self.assertEqual(seen["validation_mode"], "smoke")
        self.assertEqual(seen["test_policy"], "skip")
        self.assertEqual(seen["phase"], "build")

    def test_run_logged_command_emits_quiet_heartbeat_and_stuck_state(self):
        seen: list[dict] = []

        def report_progress(**fields):
            seen.append(dict(fields))

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                "import time; time.sleep(0.18); print('done')",
            ],
            report_progress=report_progress,
            heartbeat_interval_secs=0.05,
            stuck_idle_secs=0.1,
        )

        self.assertEqual(result["returncode"], 0)
        heartbeat_events = [item for item in seen if item.get("last_heartbeat_at")]
        self.assertTrue(heartbeat_events, msg=f"missing heartbeat events in {seen}")
        self.assertTrue(
            any(item.get("liveness") == "quiet" for item in heartbeat_events),
            msg=f"missing quiet heartbeat in {heartbeat_events}",
        )
        self.assertTrue(
            any(item.get("liveness") == "stuck" for item in heartbeat_events),
            msg=f"missing stuck heartbeat in {heartbeat_events}",
        )
        self.assertTrue(any(item.get("last_output_at") for item in seen))

    def test_run_logged_command_replaces_invalid_utf8_bytes(self):
        log_path = self.state_dir / "nonutf8.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                (
                    "import sys; "
                    "sys.stdout.buffer.write(b'prefix\\xe5suffix\\\\n'); "
                    "sys.stdout.flush()"
                ),
            ],
            log_path=log_path,
        )

        self.assertEqual(result["returncode"], 0)
        self.assertIn("prefix", result["output"])
        self.assertIn("suffix", result["output"])
        self.assertIn("\ufffd", result["output"])
        logged = log_path.read_text()
        self.assertIn("prefix", logged)
        self.assertIn("suffix", logged)

    def test_target_log_path_uses_machine_global_logs_dir(self):
        path = self.mod.target_log_path("job123", "windows")
        self.assertEqual(path, self.state_dir / "logs" / "job123" / "windows.log")

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

    def test_load_result_backfills_default_provenance(self):
        result_path = self.state_dir / "legacy-result.json"
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(
            json.dumps(
                {
                    "job_id": "job123",
                    "branch": "feature/legacy",
                    "sha": "b" * 40,
                    "results": [],
                    "overall": "pass",
                }
            )
        )

        result = self.mod.load_result(result_path)
        self.assertEqual(result["provenance"]["execution_kind"], "direct")
        self.assertEqual(result["provenance"]["direct_backend"], "local-ci")

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

    def test_format_ci_comment_includes_execution_summary(self):
        comment = self.mod.format_ci_comment(
            {
                "job_id": "job123",
                "branch": "feature/comment",
                "sha": "d" * 40,
                "completed_at": "2026-04-04T12:00:00+00:00",
                "overall": "pass",
                "results": [{"target": "mac", "status": "pass", "duration_secs": 12}],
            }
        )

        self.assertIn("Execution: `direct via local-ci`", comment)

    def test_resolve_github_actions_settings_reads_optional_config_defaults(self):
        settings = self.mod.resolve_github_actions_settings(self.mod.load_optional_config())
        self.assertEqual(settings["repository"], "danielraffel/pulp")
        self.assertEqual(settings["workflow"], "build")
        self.assertEqual(settings["provider"], "github-hosted")
        self.assertEqual(settings["wait_poll_secs"], 5)
        self.assertEqual(settings["match_timeout_secs"], 30)

    def test_resolve_workflow_runner_selector_json_reads_docs_check_provider_default(self):
        selector = self.mod.resolve_workflow_runner_selector_json(
            self.mod.load_optional_config(), "docs-check", "namespace"
        )
        self.assertEqual(selector, "\"namespace-profile-default\"")

    def test_resolve_default_provider_for_workflow_falls_back_to_github_hosted(self):
        provider, source = self.mod.resolve_default_provider_for_workflow(
            {"provider": "namespace"},
            "validate",
        )
        self.assertEqual(provider, "github-hosted")
        self.assertIn("workflow fallback", source)

    def test_resolve_workflow_field_value_and_source_reads_repo_variable_fallback(self):
        config = json.loads(self.config_path.read_text())
        del config["github_actions"]["workflows"]["docs-check"]["providers"]["namespace"]["runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        value, source = self.mod.resolve_workflow_field_value_and_source(
            self.mod.load_optional_config(),
            {"PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-repo-var\""},
            "docs-check",
            "namespace",
            "runner_selector_json",
        )
        self.assertEqual(value, "\"namespace-profile-repo-var\"")
        self.assertEqual(source, "repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON")

    def test_resolve_workflow_dispatch_field_values_reads_build_namespace_defaults(self):
        fields = self.mod.resolve_workflow_dispatch_field_values(
            self.mod.load_optional_config(),
            "build",
            "namespace",
            ["linux_runner_selector_json", "windows_runner_selector_json"],
        )
        self.assertEqual(
            fields,
            {
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )

    def test_cloud_record_round_trip_and_lookup(self):
        first = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/docs",
                "provider_requested": "namespace",
                "status": "in_progress",
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )
        second = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "fed654cba321",
                "workflow_key": "build",
                "requested_ref": "feature/build",
                "provider_requested": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "run_id": 12345,
                "dispatched_at": "2026-04-04T12:02:00+00:00",
                "updated_at": "2026-04-04T12:03:00+00:00",
            }
        )

        self.mod.save_cloud_record(first)
        self.mod.save_cloud_record(second)

        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["dispatch_id"], "fed654cba321")
        self.assertEqual(self.mod.find_cloud_record(records, "latest")["dispatch_id"], "fed654cba321")
        self.assertEqual(self.mod.find_cloud_record(records, "abc123")["dispatch_id"], "abc123def456")
        self.assertEqual(self.mod.find_cloud_record(records, "12345")["dispatch_id"], "fed654cba321")

    def test_update_cloud_record_from_run_derives_timing(self):
        record = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/docs",
                "provider_requested": "namespace",
                "dispatched_at": "2026-04-04T12:00:00+00:00",
            }
        )
        snapshot = {
            "databaseId": 98765,
            "workflowName": "Docs Consistency",
            "headBranch": "feature/docs",
            "headSha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:30+00:00",
            "jobs": [
                {
                    "name": "Resolve provider",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:06+00:00",
                    "completedAt": "2026-04-04T12:00:08+00:00",
                },
                {
                    "name": "Validate docs consistency",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:09+00:00",
                    "completedAt": "2026-04-04T12:00:30+00:00",
                },
            ],
        }

        updated = self.mod.update_cloud_record_from_run(record, snapshot, provider_resolved="namespace")

        self.assertEqual(updated["started_at"], "2026-04-04T12:00:06+00:00")
        self.assertEqual(updated["completed_at"], "2026-04-04T12:00:30+00:00")
        self.assertEqual(updated["queue_delay_secs"], 1.0)
        self.assertEqual(updated["duration_secs"], 24.0)

    def test_update_cloud_record_from_run_clears_completed_at_when_still_running(self):
        record = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "build",
                "requested_ref": "feature/build",
                "provider_requested": "namespace",
                "completed_at": "2026-04-04T12:00:30+00:00",
            }
        )
        snapshot = {
            "databaseId": 98765,
            "workflowName": "Build and Test",
            "headBranch": "feature/build",
            "headSha": "a" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:06+00:00",
            "jobs": [
                {
                    "name": "resolve-provider",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:10+00:00",
                    "completedAt": "2026-04-04T12:00:12+00:00",
                },
                {
                    "name": "Linux (x64) [namespace]",
                    "status": "in_progress",
                    "conclusion": "",
                    "startedAt": "2026-04-04T12:00:15+00:00",
                    "completedAt": "0001-01-01T00:00:00Z",
                    "steps": [
                        {
                            "startedAt": "2026-04-04T12:00:20+00:00",
                            "completedAt": "0001-01-01T00:00:00Z",
                        }
                    ],
                },
            ],
        }

        updated = self.mod.update_cloud_record_from_run(record, snapshot, provider_resolved="namespace")

        self.assertEqual(updated["completed_at"], "")
        self.assertEqual(updated["duration_secs"], 10.0)

    def test_cloud_record_summary_includes_duration(self):
        summary = self.mod.cloud_record_summary(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/docs",
                "provider_requested": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "duration_secs": 84,
            }
        )
        self.assertIn("duration=1m24s", summary)

    def test_summarize_cloud_timing_uses_latest_step_timestamp_for_in_progress_run(self):
        timing = self.mod.summarize_cloud_timing(
            {
                "status": "in_progress",
                "createdAt": "2026-04-04T12:00:05+00:00",
                "updatedAt": "2026-04-04T12:00:06+00:00",
                "jobs": [
                    {
                        "startedAt": "2026-04-04T12:00:10+00:00",
                        "completedAt": "0001-01-01T00:00:00Z",
                        "steps": [
                            {
                                "startedAt": "2026-04-04T12:00:25+00:00",
                                "completedAt": "0001-01-01T00:00:00Z",
                            }
                        ],
                    }
                ],
            }
        )

        self.assertEqual(timing["started_at"], "2026-04-04T12:00:10+00:00")
        self.assertEqual(timing["completed_at"], "")
        self.assertEqual(timing["duration_secs"], 15.0)

    def test_cmd_cloud_workflows_lists_supported_providers(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_workflows(SimpleNamespace())

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("build", output)
        self.assertIn("docs-check", output)
        self.assertIn("namespace", output)

    def test_cmd_cloud_status_reports_empty_state_cleanly(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_status(SimpleNamespace(identifier=None, refresh=False, limit=5))

        self.assertEqual(exit_code, 0)
        self.assertIn("No tracked cloud runs yet.", buf.getvalue())

    def test_cmd_cloud_namespace_doctor_reports_missing_cli(self):
        original_version = self.mod.nsc_version
        self.mod.nsc_version = lambda: None
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_namespace_doctor(SimpleNamespace())
        finally:
            self.mod.nsc_version = original_version

        self.assertEqual(exit_code, 1)
        output = buf.getvalue()
        self.assertIn("Namespace CLI: missing", output)
        self.assertIn("nsc login", output)

    def test_cmd_cloud_namespace_doctor_reports_ready_workspace(self):
        original_version = self.mod.nsc_version
        original_logged_in = self.mod.nsc_logged_in
        original_workspace_info = self.mod.nsc_workspace_info
        self.mod.nsc_version = lambda: "v0.0.493"
        self.mod.nsc_logged_in = lambda: True
        self.mod.nsc_workspace_info = lambda: {
            "Name": "Personal",
            "Tenant ID": "tenant_123",
            "Registry URL": "nscr.io/example",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_namespace_doctor(SimpleNamespace())
        finally:
            self.mod.nsc_version = original_version
            self.mod.nsc_logged_in = original_logged_in
            self.mod.nsc_workspace_info = original_workspace_info

        self.assertEqual(exit_code, 0)
        output = buf.getvalue()
        self.assertIn("Namespace CLI: ok (v0.0.493)", output)
        self.assertIn("Namespace login: ok", output)
        self.assertIn("Workspace: Personal", output)
        self.assertIn("Tenant ID: tenant_123", output)
        self.assertIn("Registry URL: nscr.io/example", output)

    def test_cmd_cloud_namespace_setup_invokes_login_then_reports_ready(self):
        original_available = self.mod.nsc_available
        original_logged_in = self.mod.nsc_logged_in
        original_run = self.mod.nsc_run
        original_version = self.mod.nsc_version
        original_workspace_info = self.mod.nsc_workspace_info

        login_checks = iter([False, True])
        calls = []
        self.mod.nsc_available = lambda: True
        self.mod.nsc_logged_in = lambda: next(login_checks)
        self.mod.nsc_run = lambda args, capture_output=True: calls.append((tuple(args), capture_output)) or SimpleNamespace(returncode=0)
        self.mod.nsc_version = lambda: "v0.0.493"
        self.mod.nsc_workspace_info = lambda: {"Name": "Personal"}
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_namespace_setup(SimpleNamespace())
        finally:
            self.mod.nsc_available = original_available
            self.mod.nsc_logged_in = original_logged_in
            self.mod.nsc_run = original_run
            self.mod.nsc_version = original_version
            self.mod.nsc_workspace_info = original_workspace_info

        self.assertEqual(exit_code, 0)
        self.assertEqual(calls, [(("login",), False)])
        output = buf.getvalue()
        self.assertIn("Namespace login: starting `nsc login`...", output)
        self.assertIn("Workspace: Personal", output)

    def test_cmd_cloud_run_rejects_unsupported_provider(self):
        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("does not support provider", output)

    def test_cmd_cloud_run_build_namespace_dispatches_selector_fields(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso
        original_repo_variables = self.mod.gh_repo_variables

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_repo_variables = lambda repository: {}
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso
            self.mod.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["dispatch_fields"]["linux_runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["dispatch_fields"]["windows_runner_selector_json"], "\"namespace-profile-default\"")

    def test_cmd_cloud_run_build_namespace_uses_repo_variable_selector_defaults(self):
        config = json.loads(self.config_path.read_text())
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["linux_runner_selector_json"]
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["windows_runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso
        original_repo_variables = self.mod.gh_repo_variables

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux-repo\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows-repo\"",
        }
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso
            self.mod.gh_repo_variables = original_repo_variables

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

        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-macos\"",
        )

    def test_cmd_cloud_run_build_cli_override_adds_one_off_macos_selector(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-big-apple\"",
        )

    def test_cmd_cloud_run_rejects_build_leg_override_for_docs_check(self):
        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("--macos-runner-selector-json is not supported", output)

    def test_cmd_cloud_run_dispatches_waits_and_persists_record(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_view = self.mod.gh_run_view
        original_sleep = self.mod.time.sleep
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
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
        self.mod.gh_run_view = lambda repository, run_id: {
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
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.gh_run_view = original_view
            self.mod.time.sleep = original_sleep
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(dispatched["workflow_file"], "docs-check.yml")
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["run_id"], 98765)
        self.assertEqual(records[0]["provider_resolved"], "namespace")
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["conclusion"], "success")

    def test_cmd_cloud_run_explicit_runner_selector_overrides_config_default(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-big-apple\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-big-apple\"")

    def test_cmd_cloud_status_shows_runner_selector(self):
        self.mod.save_cloud_record(
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
            exit_code = self.mod.cmd_cloud_status(
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

        original_gh_available = self.mod.gh_available
        original_repo_variables = self.mod.gh_repo_variables
        self.mod.gh_available = lambda: True
        self.mod.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-docs\"",
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows\"",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_repo_variables = original_repo_variables

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("configured default provider: namespace", output)
        self.assertIn("billing estimates: USD period-day=1 (estimated; verify provider pricing)", output)
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

        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: False
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.mod.gh_available = original_gh_available

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

        summary = self.mod.estimate_cloud_record_cost(
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

        self.mod.save_cloud_record(
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

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_history(
                SimpleNamespace(workflow=None, provider=None, limit=10)
            )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("cost=est $0.50", output)
        self.assertIn("period cost: est $0.50 over 1 run(s); estimated; verify provider pricing", output)

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

        self.mod.save_cloud_record(
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
        self.mod.save_cloud_record(
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
            exit_code = self.mod.cmd_cloud_compare(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("github-hosted: runs=1 success=1/1 median_elapsed=3m00s median_queue=15s median_cost=est $0.03", output)
        self.assertIn("namespace: runs=1 success=1/1 median_elapsed=2m00s median_queue=5s median_provider_time=1h00m00s median_cost=est $0.50", output)
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_recommend_prefers_fastest_observed_provider(self):
        self.mod.save_cloud_record(
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
        self.mod.save_cloud_record(
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
            exit_code = self.mod.cmd_cloud_recommend(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Recommended provider for build: namespace (fastest observed median)", output)
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_run_wait_fails_when_refresh_cannot_fetch_github_state(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_view = self.mod.gh_run_view
        original_sleep = self.mod.time.sleep
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_workflow_dispatch = lambda repository, workflow_file, ref, fields: None
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
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
        self.mod.gh_run_view = lambda repository, run_id: None
        self.mod.time.sleep = lambda _: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
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
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.gh_run_view = original_view
            self.mod.time.sleep = original_sleep
            self.mod.now_iso = original_now_iso

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("Error: Failed to refresh GitHub run 98765 from danielraffel/pulp.", output)

    def test_cmd_status_includes_recent_cloud_summary(self):
        self.mod.save_cloud_record(
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
        self.mod.save_cloud_record(
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

        original_gh_available = self.mod.gh_available
        original_view = self.mod.gh_run_view
        seen = {}
        self.mod.gh_available = lambda: True
        self.mod.gh_run_view = lambda repository, run_id: (
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
                exit_code = self.mod.cmd_cloud_status(
                    SimpleNamespace(identifier="latest", refresh=True, limit=5)
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_run_view = original_view

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
            self.mod.save_cloud_record(
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
        self.assertIn("period cost: est $3.00 over 6 run(s); estimated; verify provider pricing", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)

    def test_collect_local_ci_cleanup_plan_selects_stale_artifacts(self):
        queue = [
            {
                "id": "pending123456",
                "branch": "feature/pending",
                "sha": "a" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:00:00+00:00",
                "status": "pending",
                "fingerprint": "pending",
                "mode": "run",
                "validation": "full",
            },
            {
                "id": "keep12345678",
                "branch": "feature/keep",
                "sha": "b" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:01:00+00:00",
                "completed_at": "2026-04-04T12:02:00+00:00",
                "status": "completed",
                "fingerprint": "keep",
                "mode": "run",
                "validation": "full",
            },
        ]
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked(queue)

        (self.state_dir / "bundles").mkdir(parents=True, exist_ok=True)
        (self.state_dir / "bundles" / "pending123456.bundle").write_bytes(b"live")
        (self.state_dir / "bundles" / "stale1234567.bundle").write_bytes(b"stale")

        keep_log_dir = self.state_dir / "logs" / "keep12345678"
        keep_log_dir.mkdir(parents=True, exist_ok=True)
        (keep_log_dir / "mac.log").write_text("keep")
        stale_log_dir = self.state_dir / "logs" / "stale1234567"
        stale_log_dir.mkdir(parents=True, exist_ok=True)
        (stale_log_dir / "mac.log").write_text("stale")

        (self.state_dir / "results").mkdir(parents=True, exist_ok=True)
        (self.state_dir / "results" / "20260404-120000-keep12345678-feature-keep.json").write_text("{}\n")
        (self.state_dir / "results" / "20260404-120100-stale1234567-feature-stale.json").write_text("{}\n")

        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        plan = self.mod.collect_local_ci_cleanup_plan(
            queue,
            keep_results=0,
            keep_logs=0,
            keep_bundles=0,
            include_prepared=True,
        )

        bundle_paths = {Path(entry["path"]).name for entry in plan["categories"]["bundles"]}
        log_paths = {Path(entry["path"]).name for entry in plan["categories"]["logs"]}
        result_paths = {Path(entry["path"]).name for entry in plan["categories"]["results"]}
        prepared_paths = {str(Path(entry["path"]).relative_to(self.state_dir)) for entry in plan["categories"]["prepared"]}

        self.assertEqual(bundle_paths, {"stale1234567.bundle"})
        self.assertEqual(log_paths, {"stale1234567"})
        self.assertEqual(result_paths, {"20260404-120100-stale1234567-feature-stale.json"})
        self.assertEqual(prepared_paths, {"prepared/mac/full"})

    def test_cmd_cleanup_dry_run_preserves_files_and_reports_prepared_consequence(self):
        queue = []
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked(queue)

        (self.state_dir / "bundles").mkdir(parents=True, exist_ok=True)
        stale_bundle = self.state_dir / "bundles" / "stale1234567.bundle"
        stale_bundle.write_bytes(b"stale")
        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cleanup(
                SimpleNamespace(
                    apply=False,
                    include_prepared=True,
                    keep_results=0,
                    keep_logs=0,
                    keep_bundles=0,
                )
            )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertTrue(stale_bundle.exists())
        self.assertTrue(prepared_full.exists())
        self.assertIn("dry run only; re-run with --apply", output)
        self.assertIn("prepared cleanup removes cached build/install state", output)
        self.assertIn("Local CI footprint:", output)

    def test_cmd_cleanup_apply_removes_stale_artifacts_and_preserves_retained_state(self):
        queue = [
            {
                "id": "pending123456",
                "branch": "feature/pending",
                "sha": "a" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:00:00+00:00",
                "status": "pending",
                "fingerprint": "pending",
                "mode": "run",
                "validation": "full",
            },
            {
                "id": "keep12345678",
                "branch": "feature/keep",
                "sha": "b" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:01:00+00:00",
                "completed_at": "2026-04-04T12:02:00+00:00",
                "status": "completed",
                "fingerprint": "keep",
                "mode": "run",
                "validation": "full",
            },
        ]
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked(queue)

        (self.state_dir / "bundles").mkdir(parents=True, exist_ok=True)
        live_bundle = self.state_dir / "bundles" / "pending123456.bundle"
        live_bundle.write_bytes(b"live")
        stale_bundle = self.state_dir / "bundles" / "stale1234567.bundle"
        stale_bundle.write_bytes(b"stale")

        keep_log_dir = self.state_dir / "logs" / "keep12345678"
        keep_log_dir.mkdir(parents=True, exist_ok=True)
        (keep_log_dir / "mac.log").write_text("keep")
        stale_log_dir = self.state_dir / "logs" / "stale1234567"
        stale_log_dir.mkdir(parents=True, exist_ok=True)
        (stale_log_dir / "mac.log").write_text("stale")

        (self.state_dir / "results").mkdir(parents=True, exist_ok=True)
        keep_result = self.state_dir / "results" / "20260404-120000-keep12345678-feature-keep.json"
        keep_result.write_text("{}\n")
        stale_result = self.state_dir / "results" / "20260404-120100-stale1234567-feature-stale.json"
        stale_result.write_text("{}\n")

        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cleanup(
                SimpleNamespace(
                    apply=True,
                    include_prepared=True,
                    keep_results=0,
                    keep_logs=0,
                    keep_bundles=0,
                )
            )

        self.assertEqual(exit_code, 0)
        self.assertTrue(live_bundle.exists())
        self.assertTrue(keep_log_dir.exists())
        self.assertTrue(keep_result.exists())
        self.assertFalse(stale_bundle.exists())
        self.assertFalse(stale_log_dir.exists())
        self.assertFalse(stale_result.exists())
        self.assertFalse(prepared_full.exists())

    def test_cmd_cleanup_apply_refuses_to_run_while_job_is_running(self):
        running_queue = [
            {
                "id": "running12345",
                "branch": "feature/running",
                "sha": "c" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:00:00+00:00",
                "status": "running",
                "fingerprint": "running",
                "mode": "run",
                "validation": "full",
            }
        ]
        stale_bundle = self.state_dir / "bundles" / "stale1234567.bundle"
        stale_bundle.parent.mkdir(parents=True, exist_ok=True)
        stale_bundle.write_bytes(b"stale")

        original_load_queue = self.mod.load_queue
        self.mod.load_queue = lambda: list(running_queue)
        buf = io.StringIO()
        try:
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cleanup(
                    SimpleNamespace(
                        apply=True,
                        dry_run=False,
                        include_prepared=False,
                        keep_results=0,
                        keep_logs=0,
                        keep_bundles=0,
                    )
                )
        finally:
            self.mod.load_queue = original_load_queue

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertTrue(stale_bundle.exists())
        self.assertIn("blocked while local CI jobs are running", output)

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

    def test_cmd_status_reports_local_ci_footprint(self):
        bundle_path = self.state_dir / "bundles" / "job123456789.bundle"
        bundle_path.parent.mkdir(parents=True, exist_ok=True)
        bundle_path.write_bytes(b"bundle")
        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

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
        self.assertIn("Local CI footprint: total=", output)
        self.assertIn("bundles:", output)
        self.assertIn("prepared:", output)


if __name__ == "__main__":
    unittest.main()
