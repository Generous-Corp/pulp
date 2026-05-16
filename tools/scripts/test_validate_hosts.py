#!/usr/bin/env python3
"""Focused unit coverage for tools/deps/validate_hosts.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parents[1] / "deps" / "validate_hosts.py"
spec = importlib.util.spec_from_file_location("validate_hosts", SCRIPT)
assert spec and spec.loader
vh = importlib.util.module_from_spec(spec)
sys.modules["validate_hosts"] = vh
spec.loader.exec_module(vh)


@contextlib.contextmanager
def argv(args: list[str]):
    old = sys.argv[:]
    sys.argv = args[:]
    try:
        yield
    finally:
        sys.argv = old


class ConfigTests(unittest.TestCase):
    def test_load_config_defaults_to_empty_host_lists_when_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = pathlib.Path(td) / "hosts.local.json"

            self.assertEqual(
                vh.load_config(missing),
                {"unix_targets": [], "windows_targets": []},
            )

    def test_load_config_reads_json_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "hosts.json"
            config.write_text(
                json.dumps(
                    {
                        "unix_targets": [{"host": "linux", "path": "/repo"}],
                        "windows_targets": [{"host": "win", "path": "C:\\repo"}],
                    }
                ),
                encoding="utf-8",
            )

            self.assertEqual(vh.load_config(config)["unix_targets"][0]["host"], "linux")
            self.assertEqual(vh.load_config(config)["windows_targets"][0]["host"], "win")

    def test_load_config_preserves_extra_keys_for_future_callers(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "hosts.json"
            config.write_text(
                json.dumps(
                    {
                        "unix_targets": [],
                        "windows_targets": [],
                        "notes": {"owner": "release"},
                    }
                ),
                encoding="utf-8",
            )

            self.assertEqual(vh.load_config(config)["notes"], {"owner": "release"})


class SubprocessTests(unittest.TestCase):
    def test_current_branch_returns_stripped_git_output(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            0,
            stdout="feature/test\n",
            stderr="",
        )

        with mock.patch.object(vh.subprocess, "run", return_value=completed) as run:
            self.assertEqual(vh.current_branch(), "feature/test")

        run.assert_called_once_with(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=vh.ROOT,
            capture_output=True,
            text=True,
            check=True,
        )

    def test_run_reports_success_and_failure(self) -> None:
        calls: list[list[str]] = []

        def fake_run(cmd: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess:
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0 if cmd[0] == "ok" else 7)

        out = io.StringIO()
        with mock.patch.object(vh.subprocess, "run", side_effect=fake_run):
            with contextlib.redirect_stdout(out):
                self.assertTrue(vh.run("success", ["ok"]))
                self.assertFalse(vh.run("failure", ["bad"]))

        self.assertEqual(calls, [["ok"], ["bad"]])
        text = out.getvalue()
        self.assertIn("success: OK", text)
        self.assertIn("failure: FAILED (7)", text)

    def test_run_uses_repo_root_for_subprocess(self) -> None:
        with mock.patch.object(
            vh.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["true"], 0),
        ) as run:
            self.assertTrue(vh.run("rooted", ["true"]))

        run.assert_called_once_with(["true"], cwd=vh.ROOT)


class RemoteCommandTests(unittest.TestCase):
    def test_unix_remote_command_quotes_repo_and_branch_and_can_skip_tests(self) -> None:
        command = vh.unix_remote_command("/tmp/repo with spaces", "feature/a b", True)
        with_tests = vh.unix_remote_command("/tmp/repo", "main", False)

        self.assertIn("cd '/tmp/repo with spaces'", command)
        self.assertIn("git ls-remote --exit-code --heads origin 'feature/a b'", command)
        self.assertIn("./validate-build.sh --quiet --no-tests", command)
        self.assertIn("./validate-build.sh --quiet", with_tests)
        self.assertNotIn("--no-tests", with_tests)

    def test_windows_remote_command_escapes_values_and_can_run_tests(self) -> None:
        command = vh.windows_remote_command("C:\\repos\\pulp's", "feature/one'two", False)

        self.assertIn("$repo='C:\\repos\\pulp''s'", command)
        self.assertIn("$branch='feature/one''two'", command)
        self.assertIn("-NoTests:$false", command)

    def test_windows_remote_command_can_skip_tests(self) -> None:
        command = vh.windows_remote_command("C:\\repo", "main", True)

        self.assertIn("-NoTests:$true", command)
        self.assertIn("validate-build.ps1", command)


class MainTests(unittest.TestCase):
    def test_main_builds_local_unix_and_windows_commands_from_config(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "hosts.json"
            config.write_text(
                json.dumps(
                    {
                        "unix_targets": [{"host": "linux-host", "path": "/srv/pulp"}],
                        "windows_targets": [{"host": "win-host", "path": "C:\\pulp"}],
                    }
                ),
                encoding="utf-8",
            )
            observed: list[tuple[str, list[str]]] = []

            def fake_run(label: str, cmd: list[str]) -> bool:
                observed.append((label, cmd))
                return True

            with mock.patch.object(vh, "run", side_effect=fake_run):
                with argv(
                    [
                        "validate_hosts.py",
                        "--config",
                        str(config),
                        "--branch",
                        "local/test",
                        "--skip-tests",
                    ]
                ):
                    rc = vh.main()

        self.assertEqual(rc, 0)
        self.assertEqual(
            [label for label, _ in observed],
            ["local", "ssh linux-host", "ssh win-host"],
        )
        self.assertEqual(
            observed[0][1],
            [
                "bash",
                "./validate-build.sh",
                "--quiet",
                "--ref",
                "local/test",
                "--no-tests",
            ],
        )
        self.assertEqual(observed[1][1][:4], ["ssh", "-o", "BatchMode=yes", "linux-host"])
        self.assertIn("cd /srv/pulp", observed[1][1][4])
        self.assertIn("./validate-build.sh --quiet --no-tests", observed[1][1][4])
        self.assertEqual(
            observed[2][1][:8],
            [
                "ssh",
                "-o",
                "BatchMode=yes",
                "win-host",
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
            ],
        )
        self.assertIn("-NoTests:$true", observed[2][1][-1])

    def test_main_returns_failure_when_any_lane_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "hosts.json"
            config.write_text(
                json.dumps({"unix_targets": [], "windows_targets": []}),
                encoding="utf-8",
            )

            with mock.patch.object(vh, "run", return_value=False):
                with argv(
                    ["validate_hosts.py", "--config", str(config), "--branch", "local/test"]
                ):
                    rc = vh.main()

        self.assertEqual(rc, 1)

    def test_main_continues_after_local_failure_to_collect_remote_results(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "hosts.json"
            config.write_text(
                json.dumps(
                    {
                        "unix_targets": [{"host": "linux-host", "path": "/srv/pulp"}],
                        "windows_targets": [{"host": "win-host", "path": "C:\\pulp"}],
                    }
                ),
                encoding="utf-8",
            )
            labels: list[str] = []

            def fake_run(label: str, cmd: list[str]) -> bool:
                labels.append(label)
                return label != "local"

            with mock.patch.object(vh, "run", side_effect=fake_run):
                with argv(
                    ["validate_hosts.py", "--config", str(config), "--branch", "local/test"]
                ):
                    rc = vh.main()

        self.assertEqual(rc, 1)
        self.assertEqual(labels, ["local", "ssh linux-host", "ssh win-host"])

    def test_main_uses_current_branch_when_branch_argument_is_absent(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "missing-hosts.json"
            observed: list[tuple[str, list[str]]] = []

            def fake_run(label: str, cmd: list[str]) -> bool:
                observed.append((label, cmd))
                return True

            with mock.patch.object(vh, "current_branch", return_value="feature/current"), \
                 mock.patch.object(vh, "run", side_effect=fake_run):
                with argv(["validate_hosts.py", "--config", str(config)]):
                    rc = vh.main()

        self.assertEqual(rc, 0)
        self.assertEqual(len(observed), 1)
        self.assertEqual(observed[0][0], "local")
        self.assertEqual(observed[0][1], [
            "bash",
            "./validate-build.sh",
            "--quiet",
            "--ref",
            "feature/current",
        ])

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = pathlib.Path(td) / "hosts.json"
            config.write_text(
                json.dumps({"unix_targets": [], "windows_targets": []}),
                encoding="utf-8",
            )

            with mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess(["bash"], 0),
            ):
                with argv(
                    ["validate_hosts.py", "--config", str(config), "--branch", "local/test"]
                ):
                    with self.assertRaises(SystemExit) as ctx:
                        runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(ctx.exception.code, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
