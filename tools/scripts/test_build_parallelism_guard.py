#!/usr/bin/env python3
"""Tests for build_parallelism_guard.py."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "build_parallelism_guard", HERE / "build_parallelism_guard.py")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


def scan(text: str, suffix: str = ".sh") -> list[tuple[int, str]]:
    with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as fh:
        fh.write(text)
        path = Path(fh.name)
    try:
        return guard.scan_file(path)
    finally:
        path.unlink()


class BuildParallelismGuardTest(unittest.TestCase):
    def test_bare_parallel_is_flagged(self):
        self.assertTrue(scan("cmake --build build --parallel\n"))

    def test_bare_dash_j_is_flagged(self):
        self.assertTrue(scan("cmake --build build -j\n"))

    def test_literal_count_is_bounded(self):
        self.assertFalse(scan("cmake --build build --parallel 8\n"))
        self.assertFalse(scan("cmake --build build -j8\n"))
        self.assertFalse(scan("cmake --build build --parallel=8\n"))

    def test_shell_expansion_is_bounded(self):
        self.assertFalse(scan("cmake --build b --parallel $(getconf _NPROCESSORS_ONLN)\n"))
        self.assertFalse(scan("cmake --build b -j$(nproc)\n"))
        self.assertFalse(scan('cmake --build b -j"$JOBS"\n'))
        self.assertFalse(scan('cmake --build b -j"${JOBS}"\n'))
        self.assertFalse(scan("cmake --build b --parallel `nproc`\n"))

    def test_cxx_concat_form_is_bounded(self):
        # The CLI builds the command as a string: "… --parallel " + jobs.
        line = '"cmake --build " + dir + " --target install --parallel " + std::to_string(jobs());\n'
        self.assertFalse(scan(line, suffix=".cpp"))

    def test_flag_name_literal_is_not_a_build_command(self):
        # `arg == "--parallel"` is a comparison, not a build invocation.
        self.assertFalse(scan('if (arg == "--parallel" || arg == "-j") {\n', suffix=".cpp"))

    def test_non_build_dash_j_is_ignored(self):
        # `date -j` is not a build tool.
        self.assertFalse(scan('date -j -f "%Y" "$stamp"\n'))

    def test_comment_mentioning_parallel_is_ignored(self):
        self.assertFalse(scan("# a note about cmake --build --parallel with no count\n"))

    def test_powershell_bare_before_semicolon_is_flagged(self):
        self.assertTrue(scan("cmake --build build --config Release --parallel; exit 0\n"))

    def test_bare_parallel_on_continuation_line_is_flagged(self):
        # The build tool is on the previous physical line; folding continuations
        # must still catch the bare --parallel.
        self.assertTrue(scan("cmake --build build \\\n  --parallel\n"))

    def test_bounded_parallel_on_continuation_line_is_ok(self):
        self.assertFalse(scan("cmake --build build \\\n  --parallel 8\n"))

    def test_continuation_reports_the_command_start_line(self):
        findings = scan("echo hi\ncmake --build build \\\n  --parallel\n")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 2)  # the `cmake --build` line, not the `--parallel` line


if __name__ == "__main__":
    unittest.main()
