#!/usr/bin/env python3
"""patched_os_name_path_lint: planted bad cases are flagged, safe ones are not."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import patched_os_name_path_lint as lint  # noqa: E402


def findings(source: str) -> list[int]:
    return [line for line, _ in lint.findings_in_source(textwrap.dedent(source))]


class FindingsTests(unittest.TestCase):
    def test_path_built_under_patch_object_is_flagged(self) -> None:
        bad = """
            import pathlib
            from unittest import mock
            def test(module):
                with mock.patch.object(module.os, "name", "nt"):
                    module.run(pathlib.Path("."))
        """
        self.assertEqual(findings(bad), [6])

    def test_bare_path_and_windows_path_are_flagged(self) -> None:
        bad = """
            from pathlib import Path, WindowsPath
            from unittest import mock
            import os
            def test():
                with mock.patch.object(os, "name", "nt"), mock.patch.object(os, "open"):
                    Path("a")
                    WindowsPath("b")
        """
        self.assertEqual(findings(bad), [7, 8])

    def test_string_target_patch_is_flagged(self) -> None:
        bad = """
            import pathlib
            from unittest import mock
            def test():
                with mock.patch("tools.thing.os.name", "nt"):
                    pathlib.Path("x")
        """
        self.assertEqual(findings(bad), [6])

    def test_pure_paths_and_paths_built_first_are_clean(self) -> None:
        good = """
            import pathlib
            from unittest import mock
            def test(module):
                here = pathlib.Path(".")
                with mock.patch.object(module.os, "name", "nt"):
                    module.run(here)
                    module.run(pathlib.PurePosixPath("."))
                    module.run(pathlib.PureWindowsPath("C:/"))
        """
        self.assertEqual(findings(good), [])

    def test_patching_to_posix_is_clean(self) -> None:
        good = """
            import pathlib
            from unittest import mock
            def test(module):
                with mock.patch.object(module.os, "name", "posix"):
                    module.run(pathlib.Path("."))
        """
        self.assertEqual(findings(good), [])


class CommandTests(unittest.TestCase):
    def test_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bad = pathlib.Path(directory) / "bad.py"
            bad.write_text("import pathlib, os\nfrom unittest import mock\n"
                           "with mock.patch.object(os, 'name', 'nt'):\n"
                           "    pathlib.Path('.')\n", encoding="utf-8")
            good = pathlib.Path(directory) / "good.py"
            good.write_text("import pathlib\np = pathlib.Path('.')\n", encoding="utf-8")
            script = HERE / "patched_os_name_path_lint.py"
            run = lambda p: subprocess.run([sys.executable, str(script), str(p)],
                                           capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(run(bad).returncode, 1)
            self.assertEqual(run(good).returncode, 0)

    def test_the_tree_is_clean(self) -> None:
        self.assertEqual(lint.main([]), 0)


if __name__ == "__main__":
    unittest.main()
