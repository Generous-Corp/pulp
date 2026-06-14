#!/usr/bin/env python3
"""Command-level Windows validation integration tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_windows_validation_integration", add_module_dir=True)


class WindowsValidationIntegrationTests(unittest.TestCase):
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

    @staticmethod
    def _repo_probe(repo_path, remote_url=None):
        return {
            "home_dir": r"C:\Users\danielraffel",
            "repo_path": repo_path,
            "repo_exists": True,
            "git_dir_exists": True,
            "origin_url": remote_url or "https://github.com/danielraffel/pulp",
            "repo_path_unsafe": False,
        }

    def _run_windows_validation(
        self,
        job,
        *,
        host="win",
        repo_path=r"C:\Pulp",
        config=None,
        run_result=None,
        platform_probe=None,
        repo_probe=None,
        capture_config=None,
        **kwargs,
    ):
        captured = {}

        def fake_run_logged_command(cmd, **command_kwargs):
            captured["cmd"] = cmd
            captured["input_text"] = command_kwargs.get("input_text", "")
            return run_result or {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        def fake_sync_bundle(sync_host, sync_job, report_progress=None, config=None):
            if capture_config is not None:
                capture_config["config"] = config
            return (f"pulp-ci-{sync_job['id']}.bundle", f"refs/pulp-ci-bundles/{sync_job['id']}")

        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(self.mod, "run_logged_command", side_effect=fake_run_logged_command))
            stack.enter_context(
                mock.patch.object(
                    self.mod,
                    "probe_windows_ssh_cmake_settings",
                    side_effect=platform_probe or (lambda target_host, generator, platform, instance: (platform, instance)),
                )
            )
            stack.enter_context(mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", side_effect=fake_sync_bundle))
            stack.enter_context(
                mock.patch.object(
                    self.mod,
                    "ensure_windows_remote_repo_checkout",
                    side_effect=repo_probe or (lambda target_host, checked_repo_path, remote_url=None, **probe_kwargs: self._repo_probe(checked_repo_path, remote_url)),
                )
            )
            result = self.mod.run_windows_ssh_validation("windows", host, repo_path, job, config=config, **kwargs)

        return result, captured

    def test_windows_validation_can_pass_generator_instance(self):
        result, captured = self._run_windows_validation(
            {"id": "job123", "branch": "feature/arm", "sha": "a" * 40},
            cmake_generator="Visual Studio 17 2022",
            cmake_platform="ARM64",
            cmake_generator_instance="C:/Program Files/Microsoft Visual Studio/2022/Community",
        )

        self.assertEqual(result["status"], "pass")
        self.assertEqual(captured["cmd"][:2], ["ssh", "win"])
        self.assertIn("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance", captured["input_text"])
        self.assertIn("$GeneratorInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Community'", captured["input_text"])
        self.assertIn("$Platform = 'ARM64'", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job123.bundle'", captured["input_text"])
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/job123'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job123", captured["input_text"])

    def test_windows_validation_rejects_missing_repo_probe_payload(self):
        result, _captured = self._run_windows_validation(
            {"id": "job123n", "branch": "feature/null-probe", "sha": "a" * 40},
            repo_probe=lambda host, repo_path, remote_url=None, **kwargs: None,
        )

        self.assertEqual(result["status"], "error")
        self.assertIn("no structured payload", result["stderr_tail"])

    def test_windows_validation_passes_config_to_bundle_upload_probe(self):
        captured_config = {"config": None}
        config = json.loads(self.config_path.read_text())
        config["targets"]["windows"]["host"] = "desktop.example.com"

        result, _captured = self._run_windows_validation(
            {"id": "job123b", "branch": "feature/arm", "sha": "b" * 40},
            host="desktop.example.com",
            config=config,
            capture_config=captured_config,
        )

        self.assertEqual(result["status"], "pass")
        self.assertIs(captured_config["config"], config)

    def test_windows_single_target_rerun_enables_prepared_reuse(self):
        result, captured = self._run_windows_validation(
            {"id": "job127", "branch": "feature/rerun", "sha": "f" * 40, "targets": ["windows"]}
        )

        self.assertEqual(result["status"], "pass")
        self.assertIn("$PreparedRoot = Join-Path $CiRoot 'prepared\\windows'", captured["input_text"])
        self.assertIn("$ReusePrepared = $true", captured["input_text"])
        self.assertIn("__PULP_PREPARED__:reused", captured["input_text"])
        self.assertIn("function Remove-DirectoryTreeRobust", captured["input_text"])
        self.assertIn("""cmd.exe /d /c ('rmdir /s /q "{0}"' -f $Path) | Out-Null""", captured["input_text"])
        self.assertIn("""$LongPath = if ($Path.StartsWith('\\\\?\\')) { $Path } else { '\\\\?\\' + $Path }""", captured["input_text"])
        self.assertIn("if (-not (Test-CommitRef $Sha)) {\n        try {\n            Invoke-Native git @('fetch', 'origin')", captured["input_text"])

    def test_windows_smoke_validation_installs_sdk_and_skips_ctest(self):
        result, captured = self._run_windows_validation(
            {"id": "job126", "branch": "feature/smoke", "sha": "e" * 40, "validation": "smoke"},
            run_result={
                "timed_out": False,
                "returncode": 0,
                "output": "__PULP_VALIDATION__:smoke\n__PULP_TEST_POLICY__:skip\nok\n",
                "duration_secs": 1.2,
            },
        )

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
        result, _captured = self._run_windows_validation(
            {"id": "job126s", "branch": "feature/smoke", "sha": "f" * 40, "validation": "smoke"}
        )

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])

    def test_windows_validation_auto_detects_platform_and_vs_instance(self):
        result, captured = self._run_windows_validation(
            {"id": "job123", "branch": "feature/arm", "sha": "b" * 40},
            platform_probe=lambda host, generator, platform, instance: (
                "ARM64",
                "C:/Program Files/Microsoft Visual Studio/2022/Community",
            ),
        )

        self.assertEqual(result["status"], "pass")
        self.assertIn("$Platform = 'ARM64'", captured["input_text"])
        self.assertIn("$GeneratorInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Community'", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job123.bundle'", captured["input_text"])

    def test_windows_validation_recovers_abandoned_host_mutex(self):
        result, captured = self._run_windows_validation(
            {"id": "job124", "branch": "feature/mutex", "sha": "c" * 40}
        )

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
        result, captured = self._run_windows_validation(
            {"id": "job125", "branch": "feature/commit-probe", "sha": "d" * 40}
        )

        self.assertEqual(result["status"], "pass")
        self.assertIn("git rev-parse --verify --quiet", captured["input_text"])

    def test_windows_validation_fetches_branch_with_explicit_refspec(self):
        result, captured = self._run_windows_validation(
            {"id": "job126", "branch": "feature/refspec", "sha": "e" * 40}
        )

        self.assertEqual(result["status"], "pass")
        self.assertIn("refs/heads/$Branch`:refs/remotes/origin/$Branch", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job126.bundle'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job126", captured["input_text"])


if __name__ == "__main__":
    unittest.main()
