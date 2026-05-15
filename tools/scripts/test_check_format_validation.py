#!/usr/bin/env python3
"""Unit tests for tools/check_format_validation.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "check_format_validation.py"

spec = importlib.util.spec_from_file_location("check_format_validation", SCRIPT)
assert spec and spec.loader
cfv = importlib.util.module_from_spec(spec)
sys.modules["check_format_validation"] = cfv
spec.loader.exec_module(cfv)


class ExtractFormatsTests(unittest.TestCase):
    def test_extract_formats_reads_nested_platform_statuses(self) -> None:
        text = """\
formats:
  vst3:
    macos: stable
    windows: usable
  clap:
    linux: experimental
production_validated:
  vst3:
    macos: [Reaper]
"""

        self.assertEqual(
            cfv.extract_formats(text),
            {
                "vst3": {"macos": "stable", "windows": "usable"},
                "clap": {"linux": "experimental"},
            },
        )

    def test_extract_formats_returns_empty_when_block_absent(self) -> None:
        self.assertEqual(cfv.extract_formats("production_validated:\n"), {})


class ExtractProductionTests(unittest.TestCase):
    def test_extract_production_reads_empty_and_populated_inline_lists(self) -> None:
        text = """\
production_validated:
  vst3:
    macos: [Reaper, "Logic Pro", 'Cubase']
    windows: []
  clap:
    linux: [Bitwig]
formats:
  vst3:
    macos: stable
"""

        self.assertEqual(
            cfv.extract_production(text),
            {
                "vst3": {
                    "macos": ["Reaper", "Logic Pro", "Cubase"],
                    "windows": [],
                },
                "clap": {"linux": ["Bitwig"]},
            },
        )

    def test_extract_production_ignores_comments_and_blank_lines(self) -> None:
        text = """\
production_validated:
  # Populated during host smoke validation.

  auv2:
    macos: []
"""

        self.assertEqual(cfv.extract_production(text), {"auv2": {"macos": []}})

    def test_extract_production_returns_empty_when_block_absent(self) -> None:
        self.assertEqual(cfv.extract_production("formats:\n"), {})


class MainTests(unittest.TestCase):
    def _run_main(self, matrix_text: str, *args: str) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as td:
            matrix = pathlib.Path(td) / "support-matrix.yaml"
            matrix.write_text(matrix_text, encoding="utf-8")
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(cfv, "MATRIX_PATH", matrix), \
                 mock.patch.object(sys, "argv", ["check_format_validation.py", *args]), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = cfv.main()
            return rc, stdout.getvalue(), stderr.getvalue()

    def test_warn_mode_reports_gaps_but_exits_zero(self) -> None:
        rc, out, err = self._run_main(
            """\
formats:
  vst3:
    macos: stable
    windows: usable
    linux: experimental
  clap:
    linux: stable
production_validated:
  vst3:
    macos: [Reaper]
    windows: []
""",
        )

        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("1 format/platform pairs host-validated", out)
        self.assertIn("1 acknowledged but empty", out)
        self.assertIn("1 missing from production_validated", out)
        self.assertIn("EMPTY:   vst3.windows is 'usable'", out)
        self.assertIn("MISSING: production_validated.clap.linux absent", out)
        self.assertNotIn("vst3.linux", out)

    def test_report_mode_returns_one_when_required_validation_is_missing(self) -> None:
        rc, out, _ = self._run_main(
            """\
formats:
  vst3:
    macos: stable
production_validated:
  vst3:
    macos: []
""",
            "--mode=report",
        )

        self.assertEqual(rc, 1)
        self.assertIn("mode=report", out)
        self.assertIn("EMPTY:   vst3.macos is 'stable'", out)

    def test_report_mode_returns_zero_when_all_required_pairs_have_hosts(self) -> None:
        rc, out, err = self._run_main(
            """\
formats:
  vst3:
    macos: stable
  clap:
    linux: usable
production_validated:
  vst3:
    macos: [Reaper]
  clap:
    linux: [Bitwig]
""",
            "--mode=report",
        )

        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("2 format/platform pairs host-validated", out)
        self.assertIn("0 acknowledged but empty", out)
        self.assertIn("0 missing from production_validated", out)

    def test_read_error_returns_two_and_writes_stderr(self) -> None:
        missing = pathlib.Path(tempfile.gettempdir()) / "pulp-missing-support-matrix.yaml"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(cfv, "MATRIX_PATH", missing), \
             mock.patch.object(sys, "argv", ["check_format_validation.py"]), \
             contextlib.redirect_stdout(stdout), \
             contextlib.redirect_stderr(stderr):
            rc = cfv.main()

        self.assertEqual(rc, 2)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("Failed to read", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
