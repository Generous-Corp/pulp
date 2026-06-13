#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().with_name("macos_terminal_runner.py")


def load_macos_terminal_runner_module():
    spec = importlib.util.spec_from_file_location("macos_terminal_runner_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class MacOSTerminalRunnerTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_macos_terminal_runner_module()

    def test_strip_and_reentry_guard(self):
        self.assertEqual(
            self.mod.strip_run_in_terminal_args(["desktop", "video", "--run-in-terminal", "mac"]),
            ["desktop", "video", "mac"],
        )
        self.assertTrue(
            self.mod.should_reinvoke_in_terminal(
                requested=True,
                sys_platform="darwin",
                environ={},
            )
        )
        self.assertFalse(
            self.mod.should_reinvoke_in_terminal(
                requested=True,
                sys_platform="darwin",
                environ={self.mod.TERMINAL_REENTRY_ENV: "1"},
            )
        )
        self.assertFalse(
            self.mod.should_reinvoke_in_terminal(
                requested=True,
                sys_platform="linux",
                environ={},
            )
        )

    def test_terminal_shell_script_quotes_and_sets_reentry_env(self):
        script = self.mod.terminal_shell_script(
            cwd=Path("/repo path"),
            python_executable="/usr/bin/python3",
            script_path=Path("/repo path/tools/local-ci/local_ci.py"),
            argv=["desktop", "video-doctor", "--run-in-terminal", "mac"],
            stdout_path=Path("/tmp/out file"),
            stderr_path=Path("/tmp/err file"),
            returncode_path=Path("/tmp/rc file"),
        )

        self.assertIn("cd '/repo path'", script)
        self.assertIn("/usr/bin/caffeinate -u -t 60", script)
        self.assertIn("PULP_LOCAL_CI_TERMINAL_REENTRY=1", script)
        self.assertNotIn("--run-in-terminal", script)
        self.assertIn("desktop video-doctor mac", script)
        self.assertIn("'/tmp/out file'", script)
        self.assertIn("'/tmp/rc file'", script)

    def test_run_local_ci_in_terminal_replays_output_and_returncode(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)

            class FixedTemporaryDirectory:
                def __init__(self, *_args, **_kwargs):
                    pass

                def __enter__(self):
                    return str(tmp_path)

                def __exit__(self, *_args):
                    return False

            def fake_run(cmd, **_kwargs):
                (tmp_path / "stdout.txt").write_text("child stdout\n")
                (tmp_path / "stderr.txt").write_text("child stderr\n")
                (tmp_path / "returncode.txt").write_text("9\n")
                self.assertEqual(cmd[0], "osascript")
                self.assertIn("Terminal", cmd[-1])
                return subprocess.CompletedProcess(cmd, 0, "", "")

            with mock.patch.object(self.mod.tempfile, "TemporaryDirectory", FixedTemporaryDirectory):
                result = self.mod.run_local_ci_in_terminal(
                    ["desktop", "video", "--run-in-terminal", "mac"],
                    cwd=Path("/repo"),
                    python_executable="/usr/bin/python3",
                    script_path=Path("/repo/tools/local-ci/local_ci.py"),
                    timeout_secs=1,
                    run_fn=fake_run,
                    monotonic_fn=lambda: 0,
                    sleep_fn=lambda _secs: None,
                )

        self.assertEqual(result["returncode"], 9)
        self.assertEqual(result["stdout"], "child stdout\n")
        self.assertEqual(result["stderr"], "child stderr\n")
        self.assertFalse(result["timed_out"])

    def test_run_local_ci_in_terminal_reports_osascript_failure(self):
        result = self.mod.run_local_ci_in_terminal(
            ["desktop", "video-doctor", "mac"],
            cwd=Path("/repo"),
            python_executable="/usr/bin/python3",
            script_path=Path("/repo/tools/local-ci/local_ci.py"),
            run_fn=lambda cmd, **_kwargs: subprocess.CompletedProcess(cmd, 1, "", "denied\n"),
        )

        self.assertEqual(result["returncode"], 1)
        self.assertEqual(result["stderr"], "denied\n")


if __name__ == "__main__":
    unittest.main()
