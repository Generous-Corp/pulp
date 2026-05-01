#!/usr/bin/env python3
"""Tests for tools/check_format_validation.py."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import runpy
import sys
import tempfile
import unittest
from pathlib import Path
from textwrap import dedent
from unittest import mock


_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "check_format_validation", _HERE / "check_format_validation.py"
)
assert _SPEC and _SPEC.loader
cfv = importlib.util.module_from_spec(_SPEC)
sys.modules["check_format_validation"] = cfv
_SPEC.loader.exec_module(cfv)


SYNTHETIC_MATRIX = dedent(
    """\
    schema_version: 1
    formats:
      vst3:
        macos: usable
        windows: stable
        linux: partial
      clap:
        macos: usable
        linux: experimental
      au_v2:
        macos: unsupported
        notes: >
          Nested notes are ignored by the format parser.
    production_validated:
      vst3:
        macos: ["Reaper", 'Nuendo']
        windows: []
      clap:
        linux: [Bitwig]
    trailing_section:
      ignored: true
    """
)


class ParserTests(unittest.TestCase):
    def test_extract_formats_reads_format_platform_statuses(self) -> None:
        self.assertEqual(
            cfv.extract_formats(SYNTHETIC_MATRIX),
            {
                "vst3": {
                    "macos": "usable",
                    "windows": "stable",
                    "linux": "partial",
                },
                "clap": {
                    "macos": "usable",
                    "linux": "experimental",
                },
                "au_v2": {
                    "macos": "unsupported",
                },
            },
        )

    def test_extract_formats_returns_empty_when_block_missing(self) -> None:
        self.assertEqual(cfv.extract_formats("schema_version: 1\n"), {})

    def test_extract_production_parses_host_lists_and_empty_lists(self) -> None:
        self.assertEqual(
            cfv.extract_production(SYNTHETIC_MATRIX),
            {
                "vst3": {
                    "macos": ["Reaper", "Nuendo"],
                    "windows": [],
                },
                "clap": {
                    "linux": ["Bitwig"],
                },
            },
        )

    def test_extract_production_ignores_comments_and_missing_block(self) -> None:
        matrix = dedent(
            """\
            production_validated:
              # comment

              vst3:
                macos: [Reaper]
            """
        )

        self.assertEqual(cfv.extract_production(matrix), {"vst3": {"macos": ["Reaper"]}})
        self.assertEqual(cfv.extract_production("formats:\n  vst3:\n    macos: usable\n"), {})

    def test_extract_production_ignores_orphan_platform_lists(self) -> None:
        matrix = dedent(
            """\
            production_validated:
                macos: [Ignored]
              vst3:
                macos: [Reaper]
            """
        )

        self.assertEqual(cfv.extract_production(matrix), {"vst3": {"macos": ["Reaper"]}})


class MainTests(unittest.TestCase):
    def _run_main(self, matrix_text: str | None, mode: str = "warn") -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            matrix_path = Path(td) / "support-matrix.yaml"
            if matrix_text is not None:
                matrix_path.write_text(matrix_text, encoding="utf-8")
            with mock.patch.object(cfv, "MATRIX_PATH", matrix_path), \
                 mock.patch.object(sys, "argv", ["check_format_validation.py", "--mode", mode]), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = cfv.main()
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_warn_mode_reports_missing_and_empty_gaps_without_failing(self) -> None:
        rc, stdout, stderr = self._run_main(SYNTHETIC_MATRIX)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("1 format/platform pairs host-validated", stdout)
        self.assertIn("1 acknowledged but empty", stdout)
        self.assertIn("1 missing from production_validated (mode=warn)", stdout)
        self.assertIn("MISSING: production_validated.clap.macos absent", stdout)
        self.assertIn("EMPTY:   vst3.windows is 'stable'", stdout)

    def test_report_mode_fails_on_missing_or_empty_gaps(self) -> None:
        rc, stdout, stderr = self._run_main(SYNTHETIC_MATRIX, mode="report")

        self.assertEqual(rc, 1)
        self.assertEqual(stderr, "")
        self.assertIn("mode=report", stdout)

    def test_report_mode_passes_when_required_formats_have_hosts(self) -> None:
        matrix = dedent(
            """\
            formats:
              vst3:
                macos: usable
              clap:
                linux: stable
              lv2:
                linux: experimental
            production_validated:
              vst3:
                macos: [Reaper]
              clap:
                linux: [Bitwig]
            """
        )

        rc, stdout, stderr = self._run_main(matrix, mode="report")

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("2 format/platform pairs host-validated", stdout)
        self.assertIn("0 acknowledged but empty", stdout)
        self.assertIn("0 missing from production_validated", stdout)

    def test_missing_matrix_returns_input_error(self) -> None:
        rc, stdout, stderr = self._run_main(None)

        self.assertEqual(rc, 2)
        self.assertEqual(stdout, "")
        self.assertIn("Failed to read", stderr)

    def test_script_entrypoint_exits_with_main_result(self) -> None:
        matrix = dedent(
            """\
            formats:
              vst3:
                macos: usable
            production_validated:
              vst3:
                macos: [Reaper]
            """
        )

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", ["check_format_validation.py", "--mode", "report"]), \
             mock.patch.object(Path, "read_text", return_value=matrix), \
             contextlib.redirect_stdout(stdout), \
             contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as raised:
                runpy.run_path(str(_HERE / "check_format_validation.py"), run_name="__main__")

        self.assertEqual(raised.exception.code, 0)
        self.assertIn("1 format/platform pairs host-validated", stdout.getvalue())
        self.assertEqual(stderr.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
