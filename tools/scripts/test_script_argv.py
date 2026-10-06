#!/usr/bin/env python3
"""Controls for script_argv.argv_for on both platforms."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import script_argv


class ArgvForTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, name: str, data: bytes) -> pathlib.Path:
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_posix_launches_every_path_directly(self) -> None:
        script = self.write("fake", b"#!/usr/bin/env python3\nprint(1)\n")
        self.assertEqual(script_argv.argv_for(script, platform="posix"), [str(script)])

    def test_windows_runs_a_python_shebang_through_this_interpreter(self) -> None:
        for line in (b"#!/usr/bin/env python3", b"#!/usr/bin/python3 -u", b"#!/usr/bin/env python"):
            script = self.write("fake.py", line + b"\nprint(1)\n")
            self.assertEqual(script_argv.argv_for(script, platform="nt"),
                             [sys.executable, str(script)], line)

    def test_windows_runs_a_shell_shebang_through_bash(self) -> None:
        script = self.write("fake", b"#!/bin/sh\necho hi\n")
        with mock.patch.object(script_argv.shutil, "which", return_value="C:/Git/bin/bash.exe"):
            self.assertEqual(script_argv.argv_for(script, platform="nt"),
                             ["C:/Git/bin/bash.exe", str(script)])

    def test_windows_finds_git_bash_beside_git_when_bash_is_not_on_path(self) -> None:
        script = self.write("fake", b"#!/bin/sh\necho hi\n")
        git_root = self.root / "Git"
        (git_root / "cmd").mkdir(parents=True)
        (git_root / "bin").mkdir()
        (git_root / "cmd" / "git.exe").write_bytes(b"MZ")
        bash = git_root / "bin" / "bash.exe"
        bash.write_bytes(b"MZ")
        which = {"bash": None, "git": str(git_root / "cmd" / "git.exe")}
        with mock.patch.object(script_argv.shutil, "which", side_effect=which.get):
            self.assertEqual(script_argv.argv_for(script, platform="nt"),
                             [str(bash), str(script)])

    def test_windows_without_bash_leaves_a_shell_script_alone(self) -> None:
        script = self.write("fake", b"#!/bin/sh\necho hi\n")
        with mock.patch.object(script_argv.shutil, "which", return_value=None):
            self.assertEqual(script_argv.argv_for(script, platform="nt"), [str(script)])

    def test_windows_passes_native_images_and_missing_paths_through(self) -> None:
        native = self.write("tool.exe", b"MZ\x90\x00rest")
        self.assertEqual(script_argv.argv_for(native, platform="nt"), [str(native)])
        missing = self.root / "absent.exe"
        self.assertEqual(script_argv.argv_for(missing, platform="nt"), [str(missing)])
        self.assertEqual(script_argv.argv_for("gh", platform="nt"), ["gh"])


class ConsumersLaunchThroughArgvForTests(unittest.TestCase):
    """The code that launches a configurable tool must go through argv_for,
    or a script stand-in fails on Windows before the code under test runs."""

    def test_trace_frame_cost_runs_its_processor_through_argv_for(self) -> None:
        import trace_frame_cost

        seen: list[str] = []

        def spy(path, **kwargs):
            seen.append(str(path))
            return [sys.executable, "-c", "print('col')"]

        with tempfile.TemporaryDirectory() as directory:
            trace = pathlib.Path(directory) / "t.pftrace"
            trace.write_bytes(b"")
            with mock.patch.object(trace_frame_cost, "argv_for", side_effect=spy):
                trace_frame_cost.run_processor("/fake/processor", trace, "select 1")
        self.assertEqual(seen, ["/fake/processor"])

    def test_queue_batch_attribute_runs_gh_through_argv_for(self) -> None:
        import queue_batch_attribute

        seen: list[str] = []

        def spy(path, **kwargs):
            seen.append(str(path))
            return [sys.executable, "-c", "pass"]

        with mock.patch.object(queue_batch_attribute, "gh_cli", return_value="/fake/gh"), \
                mock.patch.object(queue_batch_attribute, "argv_for", side_effect=spy):
            queue_batch_attribute.run_gh(["api", "x"], capture_output=True)
        self.assertEqual(seen, ["/fake/gh"])


if __name__ == "__main__":
    unittest.main()
