#!/usr/bin/env python3
"""Command-level desktop source preparation integration tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_desktop_source_integration", add_module_dir=True)


class DesktopSourceIntegrationTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
