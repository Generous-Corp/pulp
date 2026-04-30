#!/usr/bin/env python3
"""Additional edge coverage for coverage_diff_comment.py."""

from __future__ import annotations

import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import coverage_diff_comment as cdc  # noqa: E402


PLAIN_REPORT = """## Diff: origin/main...HEAD
- core/runtime/src/base64.cpp (91.7%): Line 42 missing
## Summary
- **Coverage**: 91.7%
"""


class RenderPlainReportTests(unittest.TestCase):
    def test_plain_report_is_not_stripped(self) -> None:
        body = cdc.render(
            PLAIN_REPORT,
            flip_date="2026-05-05",
            threshold=80,
        )
        self.assertIn("## Diff: origin/main...HEAD", body)
        self.assertIn("core/runtime/src/base64.cpp", body)
        self.assertIn("<details>", body)


class ReadReportEdgeTests(unittest.TestCase):
    def test_directory_report_returns_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertIsNone(cdc._read_report(pathlib.Path(tmpdir)))


class ScriptEntrypointTests(unittest.TestCase):
    def test_main_guard_exits_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            report = tmp / "coverage-diff.md"
            out = tmp / "coverage-diff-comment.md"
            report.write_text(PLAIN_REPORT, encoding="utf-8")
            argv = [
                str(HERE / "coverage_diff_comment.py"),
                "--report",
                str(report),
                "--flip-date",
                "2026-05-05",
                "--threshold",
                "80",
                "--out",
                str(out),
            ]

            with mock.patch.object(sys, "argv", argv):
                with self.assertRaises(SystemExit) as raised:
                    runpy.run_path(
                        str(HERE / "coverage_diff_comment.py"),
                        run_name="__main__",
                    )

            self.assertEqual(raised.exception.code, 0)
            body = out.read_text(encoding="utf-8")
            self.assertIn(cdc.COMMENT_MARKER, body)
            self.assertIn("core/runtime/src/base64.cpp", body)

    def test_script_entrypoint_handles_unreadable_report_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            out = tmp / "coverage-diff-comment.md"

            completed = subprocess.run(
                [
                    sys.executable,
                    str(HERE / "coverage_diff_comment.py"),
                    "--report",
                    str(tmp),
                    "--flip-date",
                    "2026-05-05",
                    "--threshold",
                    "80",
                    "--out",
                    str(out),
                ],
                check=True,
                text=True,
                capture_output=True,
            )

            self.assertEqual(completed.stderr, "")
            body = out.read_text(encoding="utf-8")
            self.assertIn(cdc.COMMENT_MARKER, body)
            self.assertIn("did not touch any instrumented source", body)
            self.assertNotIn("<details>", body)

    def test_script_entrypoint_writes_required_comment(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            report = tmp / "coverage-diff.md"
            out = tmp / "coverage-diff-comment.md"
            report.write_text(PLAIN_REPORT, encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(HERE / "coverage_diff_comment.py"),
                    "--report",
                    str(report),
                    "--flip-date",
                    "2026-05-05",
                    "--threshold",
                    "80",
                    "--no-advisory",
                    "--out",
                    str(out),
                ],
                check=True,
                text=True,
                capture_output=True,
            )

            self.assertEqual(completed.stdout, "")
            body = out.read_text(encoding="utf-8")
            self.assertIn(cdc.COMMENT_MARKER, body)
            self.assertIn("## Diff coverage (required)", body)
            self.assertIn("Diff coverage threshold: **80%**", body)
            self.assertNotIn("informational only", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
