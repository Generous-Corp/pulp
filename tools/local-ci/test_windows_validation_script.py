#!/usr/bin/env python3
"""Tests for Windows validation PowerShell script construction."""

from __future__ import annotations

import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_validation_script.py")


def load_module(path: Path = MODULE_PATH, name: str = "pulp_local_ci_windows_validation_script"):
    return load_module_from_path(path, module_name=name, add_module_dir=True)


class WindowsValidationScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_builds_full_script_with_injected_policy_helpers(self) -> None:
        job = {
            "id": "job801",
            "branch": "feature/windows",
            "sha": "e" * 40,
            "targets": ["windows"],
            "validation": "full",
        }

        script, validation = self.mod.build_windows_validation_script(
            "windows",
            "win.example.com",
            r"C:\Pulp's Repo",
            job,
            bundle_name="pulp-ci-job801.bundle",
            bundle_ref="refs/pulp-ci-bundles/job801",
            exclude_tests="slow windows",
            cmake_generator="Visual Studio 17 2022",
            resolved_platform="ARM64",
            resolved_generator_instance=r"C:\VS\2022",
            ps_literal_fn=lambda value: value.replace("'", "''"),
            remote_commit_error_fn=lambda target, host, queued_job: (
                f"{target} cannot validate {queued_job['sha'][:12]} on {host}"
            ),
            should_reuse_prepared_state_fn=lambda queued_job: queued_job["targets"] == ["windows"],
        )

        self.assertEqual(validation, "full")
        self.assertIn(r"$Repo = 'C:\Pulp''s Repo'", script)
        self.assertIn("$Branch = 'feature/windows'", script)
        self.assertIn("$Sha = '" + "e" * 40 + "'", script)
        self.assertIn("$BundleName = 'pulp-ci-job801.bundle'", script)
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job801", script)
        self.assertIn("$ExcludeRegex = 'slow windows'", script)
        self.assertIn("$Generator = 'Visual Studio 17 2022'", script)
        self.assertIn("$Platform = 'ARM64'", script)
        self.assertIn(r"$GeneratorInstance = 'C:\VS\2022'", script)
        self.assertIn("$ValidationMode = 'full'", script)
        self.assertIn("$PreparedRoot = Join-Path $CiRoot 'prepared\\windows'", script)
        self.assertIn("$ReusePrepared = $true", script)
        self.assertIn('Write-Host "__PULP_TEST_POLICY__:run"', script)
        self.assertIn("windows cannot validate eeeeeeeeeeee on win.example.com", script)

    def test_builds_smoke_script_without_prepared_reuse_for_multi_target_jobs(self) -> None:
        job = {
            "id": "job802",
            "branch": "feature/smoke",
            "sha": "f" * 40,
            "targets": ["mac", "windows"],
            "validation": "smoke",
        }

        script, validation = self.mod.build_windows_validation_script(
            "windows",
            "win.example.com",
            r"C:\Pulp",
            job,
            bundle_name="pulp-ci-job802.bundle",
            bundle_ref="refs/pulp-ci-bundles/job802",
            exclude_tests="",
            cmake_generator="Visual Studio 17 2022",
            resolved_platform="",
            resolved_generator_instance="",
            ps_literal_fn=lambda value: value.replace("'", "''"),
            remote_commit_error_fn=lambda target, host, queued_job: "unavailable",
            should_reuse_prepared_state_fn=lambda queued_job: len(queued_job["targets"]) == 1,
        )

        self.assertEqual(validation, "smoke")
        self.assertIn("$ValidationMode = 'smoke'", script)
        self.assertIn("$ReusePrepared = $false", script)
        self.assertIn('Write-Host "__PULP_TEST_POLICY__:skip"', script)
        self.assertIn("-DPULP_BUILD_TESTS=OFF", script)
        self.assertIn("-DPULP_BUILD_EXAMPLES=OFF", script)
        self.assertIn("-DPULP_ENABLE_AUDIO_PROBES=OFF", script)
        self.assertIn("-DPULP_ENABLE_GPU=OFF", script)
        self.assertIn("Invoke-Native cmake @('--install', $Build, '--prefix', $Install, '--config', 'Release')", script)
        self.assertIn('Write-Host "__PULP_PHASE__:smoke"', script)


if __name__ == "__main__":
    unittest.main()
