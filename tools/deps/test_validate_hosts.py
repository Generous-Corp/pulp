#!/usr/bin/env python3
"""Focused tests for tools/deps/validate_hosts.py."""

from __future__ import annotations

import contextlib
import io
import json
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(ROOT / "tools" / "deps"))
import validate_hosts  # noqa: E402


class ConfigTests(unittest.TestCase):
    def test_load_config_missing_file_returns_empty_target_lists(self) -> None:
        missing = Path(tempfile.gettempdir()) / "pulp-missing-hosts-local.json"
        self.assertFalse(missing.exists())

        self.assertEqual(
            validate_hosts.load_config(missing),
            {"unix_targets": [], "windows_targets": []},
        )

    def test_load_config_reads_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = Path(td) / "hosts.json"
            config.write_text(
                json.dumps(
                    {
                        "unix_targets": [{"host": "mac", "path": "/repo"}],
                        "windows_targets": [{"host": "win", "path": "C:\\repo"}],
                    }
                ),
                encoding="utf-8",
            )

            self.assertEqual(
                validate_hosts.load_config(config),
                {
                    "unix_targets": [{"host": "mac", "path": "/repo"}],
                    "windows_targets": [{"host": "win", "path": "C:\\repo"}],
                },
            )


class SubprocessWrapperTests(unittest.TestCase):
    def test_current_branch_uses_repo_root_and_strips_stdout(self) -> None:
        completed = subprocess.CompletedProcess(
            args=["git"],
            returncode=0,
            stdout="feature/test\n",
            stderr="",
        )
        with mock.patch.object(validate_hosts.subprocess, "run", return_value=completed) as run:
            self.assertEqual(validate_hosts.current_branch(), "feature/test")

        self.assertEqual(
            run.call_args.args[0],
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        )
        self.assertEqual(run.call_args.kwargs["cwd"], validate_hosts.ROOT)
        self.assertTrue(run.call_args.kwargs["check"])

    def test_run_reports_success_and_failure(self) -> None:
        stdout = io.StringIO()
        with mock.patch.object(
            validate_hosts.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["cmd"], 0),
        ) as run, contextlib.redirect_stdout(stdout):
            self.assertTrue(validate_hosts.run("local", ["cmd"]))

        self.assertEqual(run.call_args.args[0], ["cmd"])
        self.assertEqual(run.call_args.kwargs["cwd"], validate_hosts.ROOT)
        self.assertIn("local: OK", stdout.getvalue())

        stdout = io.StringIO()
        with mock.patch.object(
            validate_hosts.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["cmd"], 17),
        ), contextlib.redirect_stdout(stdout):
            self.assertFalse(validate_hosts.run("remote", ["cmd"]))

        self.assertIn("remote: FAILED (17)", stdout.getvalue())


class RemoteCommandTests(unittest.TestCase):
    def test_unix_remote_command_quotes_paths_and_toggles_tests(self) -> None:
        cmd = validate_hosts.unix_remote_command(
            "/tmp/repo path",
            "feature/quote'test",
            skip_tests=True,
        )

        self.assertIn("cd '/tmp/repo path'", cmd)
        self.assertIn("'feature/quote'\"'\"'test'", cmd)
        self.assertIn("./validate-build.sh --quiet --no-tests", cmd)
        self.assertIn("git fetch origin", cmd)

        with_tests = validate_hosts.unix_remote_command("/repo", "main", skip_tests=False)
        self.assertIn("./validate-build.sh --quiet", with_tests)
        self.assertNotIn("--no-tests", with_tests)

    def test_windows_remote_command_escapes_quotes_and_toggles_tests(self) -> None:
        cmd = validate_hosts.windows_remote_command(
            "C:\\repo path\\agent's",
            "feature/agent's",
            skip_tests=True,
        )

        self.assertIn("$repo='C:\\repo path\\agent''s'", cmd)
        self.assertIn("$branch='feature/agent''s'", cmd)
        self.assertIn('-NoTests:$true', cmd)
        self.assertIn("git -C $repo fetch origin", cmd)

        with_tests = validate_hosts.windows_remote_command("C:\\repo", "main", skip_tests=False)
        self.assertIn("-NoTests:$false", with_tests)


class MainTests(unittest.TestCase):
    def test_main_runs_local_and_configured_remotes(self) -> None:
        config = {
            "unix_targets": [{"host": "linux", "path": "/repo"}],
            "windows_targets": [{"host": "windows", "path": "C:\\repo"}],
        }

        with mock.patch.object(sys, "argv", [
            "validate_hosts.py",
            "--config",
            "hosts.json",
            "--branch",
            "feature/test",
            "--skip-tests",
        ]), mock.patch.object(
            validate_hosts, "load_config", return_value=config
        ) as load_config, mock.patch.object(
            validate_hosts, "current_branch"
        ) as current_branch, mock.patch.object(
            validate_hosts, "run", side_effect=[True, False, True]
        ) as run:
            self.assertEqual(validate_hosts.main(), 1)

        load_config.assert_called_once_with(Path("hosts.json"))
        current_branch.assert_not_called()
        labels = [call.args[0] for call in run.call_args_list]
        self.assertEqual(labels, ["local", "ssh linux", "ssh windows"])

        local_cmd = run.call_args_list[0].args[1]
        self.assertEqual(
            local_cmd,
            ["bash", "./validate-build.sh", "--quiet", "--ref", "feature/test", "--no-tests"],
        )
        self.assertEqual(run.call_args_list[1].args[1][:4], ["ssh", "-o", "BatchMode=yes", "linux"])
        self.assertIn("powershell", run.call_args_list[2].args[1])

    def test_main_uses_current_branch_when_branch_argument_missing(self) -> None:
        with mock.patch.object(sys, "argv", ["validate_hosts.py"]), mock.patch.object(
            validate_hosts, "load_config", return_value={}
        ), mock.patch.object(
            validate_hosts, "current_branch", return_value="current"
        ), mock.patch.object(
            validate_hosts, "run", return_value=True
        ) as run:
            self.assertEqual(validate_hosts.main(), 0)

        self.assertEqual(
            run.call_args.args[1],
            ["bash", "./validate-build.sh", "--quiet", "--ref", "current"],
        )

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        script = ROOT / "tools" / "deps" / "validate_hosts.py"
        missing_config = Path(tempfile.gettempdir()) / "pulp-validate-hosts-missing.json"
        self.assertFalse(missing_config.exists())
        branch = subprocess.CompletedProcess(
            args=["git"],
            returncode=0,
            stdout="entrypoint-branch\n",
            stderr="",
        )
        local = subprocess.CompletedProcess(args=["bash"], returncode=0)

        with mock.patch.object(
            sys, "argv", [str(script), "--config", str(missing_config)]
        ), mock.patch.object(
            subprocess, "run", side_effect=[branch, local]
        ) as run:
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    runpy.run_path(str(script), run_name="__main__")

        self.assertEqual(raised.exception.code, 0)
        self.assertEqual(run.call_count, 2)


if __name__ == "__main__":
    unittest.main()
