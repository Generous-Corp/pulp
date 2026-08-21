#!/usr/bin/env python3
"""Hostile tests for validation build-directory serialization."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_dir_lock


class BuildDirLockTest(unittest.TestCase):
    def test_remainder_requires_a_real_command(self) -> None:
        with self.assertRaises(SystemExit):
            build_dir_lock.parse_args(["--build-dir", "build", "--"])

    @unittest.skipIf(sys.platform == "win32", "timing probe uses POSIX Python command")
    def test_two_processes_cannot_enter_the_same_build_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            events = root / "events.txt"
            child = (
                "import pathlib,sys,time; "
                "p=pathlib.Path(sys.argv[1]); tag=sys.argv[2]; "
                "p.open('a').write(tag+'-start\\n'); time.sleep(0.2); "
                "p.open('a').write(tag+'-end\\n')"
            )
            wrapper = Path(build_dir_lock.__file__).resolve()
            def command(tag: str) -> list[str]:
                return [
                    sys.executable,
                    str(wrapper),
                    "--build-dir",
                    str(build),
                    "--",
                    sys.executable,
                    "-c",
                    child,
                    str(events),
                    tag,
                ]
            first = subprocess.Popen(command("first"))
            time.sleep(0.05)
            second = subprocess.Popen(command("second"))
            self.assertEqual(first.wait(timeout=5), 0)
            self.assertEqual(second.wait(timeout=5), 0)
            self.assertEqual(
                events.read_text(encoding="utf-8").splitlines(),
                ["first-start", "first-end", "second-start", "second-end"],
            )


if __name__ == "__main__":
    unittest.main()
