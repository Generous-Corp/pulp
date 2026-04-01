#!/usr/bin/env python3

import importlib.util
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("local_ci.py")


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

    def test_claim_next_job_prefers_higher_priority(self):
        low_job, _ = self.mod.enqueue_job("feature/low", "1" * 40, "low", ["mac"], "run", "full")
        high_job, _ = self.mod.enqueue_job("feature/high", "2" * 40, "high", ["mac"], "run", "full")

        claimed = self.mod.claim_next_job()
        self.assertIsNotNone(claimed)
        self.assertEqual(claimed["id"], high_job["id"])
        self.assertNotEqual(claimed["id"], low_job["id"])

    def test_resolve_targets_uses_defaults_and_rejects_disabled_targets(self):
        config = self.mod.load_config()
        self.assertEqual(self.mod.resolve_targets(config, None), ["mac"])

        with self.assertRaises(ValueError):
            self.mod.resolve_targets(config, ["windows"])

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

    def test_windows_smoke_validation_installs_sdk_and_skips_ctest(self):
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
        self.assertIn('git fetch "$bundle" "$bundle_ref:refs/remotes/origin/$branch"', remote_cmd)

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

    def test_parse_progress_marker_detects_phase_and_wait(self):
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_PHASE__:build\n"),
            {"phase": "build"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_WAIT__:host-lock\n"),
            {"wait_reason": "host-lock"},
        )
        self.assertEqual(self.mod.parse_progress_marker("normal output\n"), {})

    def test_target_log_path_uses_machine_global_logs_dir(self):
        path = self.mod.target_log_path("job123", "windows")
        self.assertEqual(path, self.state_dir / "logs" / "job123" / "windows.log")


if __name__ == "__main__":
    unittest.main()
