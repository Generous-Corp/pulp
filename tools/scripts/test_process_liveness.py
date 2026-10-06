#!/usr/bin/env python3
"""Controls for process_liveness.pid_alive and raw_pid_probe_lint."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import process_liveness  # noqa: E402
import raw_pid_probe_lint  # noqa: E402


def dead_pid() -> int:
    proc = subprocess.Popen([sys.executable, "-c", "pass"])
    proc.wait()
    return proc.pid


class PidAliveTests(unittest.TestCase):
    def test_posix_probe_tells_live_from_dead(self) -> None:
        if os.name == "nt":
            self.skipTest("POSIX route")
        self.assertIs(process_liveness.pid_alive(os.getpid()), True)
        self.assertIs(process_liveness.pid_alive(dead_pid()), False)

    def test_invalid_pids_are_not_judged(self) -> None:
        for pid in (0, -1, None, "12", 3.0, True):
            self.assertIsNone(process_liveness.pid_alive(pid), pid)

    def test_windows_route_never_calls_os_kill(self) -> None:
        # On Windows os.kill(pid, 0) is a console-wide Ctrl+C. The Windows
        # route must reach the OpenProcess probe and never os.kill.
        asked: list[int] = []

        def probe(pid: int) -> bool:
            asked.append(pid)
            return False

        with mock.patch.object(process_liveness.os, "kill",
                               side_effect=AssertionError("os.kill on Windows")), \
                mock.patch.object(process_liveness, "_windows_pid_alive", side_effect=probe):
            self.assertIs(process_liveness.pid_alive(4242, platform="nt"), False)
        self.assertEqual(asked, [4242])


class RawPidProbeLintTests(unittest.TestCase):
    def test_flags_signal_zero_and_nothing_else(self) -> None:
        text = (
            "import os, signal\n"
            "os.kill(pid, 0)\n"
            "os.kill(pid, signal.SIGTERM)\n"
            "os.kill(int(owner['pid']), 0)\n"
            "os.kill(pid, 0)  # raw-pid-probe-lint: skip documented reason\n"
        )
        self.assertEqual(raw_pid_probe_lint.violations(text), [2, 4])

    def test_main_fails_on_a_probe_and_passes_a_clean_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "tools").mkdir()
            (root / "tools/ok.py").write_text("import os\nos.kill(1, 15)\n", encoding="utf-8")
            self.assertEqual(raw_pid_probe_lint.main(["--root", str(root)],
                                                     list_files=lambda _r: ["tools/ok.py"]), 0)
            (root / "tools/bad.py").write_text("import os\nos.kill(1, 0)\n", encoding="utf-8")
            self.assertEqual(raw_pid_probe_lint.main(
                ["--root", str(root)], list_files=lambda _r: ["tools/ok.py", "tools/bad.py"]), 1)

    def test_an_empty_tree_is_a_scan_error(self) -> None:
        self.assertEqual(raw_pid_probe_lint.main(["--root", "."], list_files=lambda _r: []), 2)


if __name__ == "__main__":
    unittest.main()
