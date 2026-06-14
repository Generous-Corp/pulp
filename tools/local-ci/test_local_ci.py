#!/usr/bin/env python3

import io
import json
import os
import subprocess
import tempfile
import unittest
from urllib.parse import urlparse
from unittest import mock
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("local_ci.py")


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

if __name__ == "__main__":
    unittest.main()
