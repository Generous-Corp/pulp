#!/usr/bin/env python3
"""Fixture tests for tools/scripts/coverage_diff_comment.py.

Guards the PR comment body formatter used by the #566 Phase 1 PR 3
diff-cover advisory gate. Pure-Python and fast — no diff-cover binary,
no git history, no network.

Run:
    python3 tools/scripts/test_coverage_diff_comment.py
"""

from __future__ import annotations

import pathlib
import runpy
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import coverage_diff_comment as cdc  # noqa: E402


SAMPLE_REPORT = """# Diff Coverage
## Diff: origin/main...HEAD
- core/audio/src/foo.cpp (82.5%): Lines 10-12, 40 missing
- core/view/src/bar.cpp (100%)
## Summary
- **Total**: 240 lines
- **Missing**: 52 lines
- **Coverage**: 78%
"""


class RenderTests(unittest.TestCase):
    def test_emits_stable_marker_for_upsert(self) -> None:
        # Workflow step greps for the marker to find a prior bot comment
        # and PATCHes it instead of spamming new comments on every run.
        body = cdc.render(
            SAMPLE_REPORT,
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertIn(cdc.COMMENT_MARKER, body)
        # Marker must be at the very top so a grep pass sees it even in a
        # raw-text context. Not strictly required for the upsert logic,
        # but the ruleset-drift-check pattern puts it first and we keep
        # parity to minimise cognitive load.
        self.assertTrue(body.lstrip().startswith(cdc.COMMENT_MARKER))

    def test_advisory_banner_mentions_flip_date_and_threshold(self) -> None:
        body = cdc.render(
            SAMPLE_REPORT,
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertIn("2026-05-05", body)
        self.assertIn("75%", body)
        self.assertIn("informational", body.lower())
        self.assertIn("currently represented", body)

    def test_required_banner_drops_advisory_language(self) -> None:
        # When Phase 3 flips the gate from advisory to required we want
        # to reuse this same renderer — exercise that mode now so the
        # future change is a one-line flag flip, not a re-render rewrite.
        body = cdc.render(
            SAMPLE_REPORT,
            flip_date="2026-05-05",
            threshold=75,
            advisory=False,
        )
        self.assertIn("required", body.lower())
        self.assertNotIn("informational", body.lower())
        self.assertIn("currently represented", body)

    def test_strips_duplicate_top_level_heading(self) -> None:
        # diff-cover emits "# Diff Coverage" at the top; the PR comment
        # already has a `##`-level section header, and double-nested
        # headings render strangely inside <details>.
        body = cdc.render(
            SAMPLE_REPORT,
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertNotIn("# Diff Coverage", body)
        # The section body should still be there.
        self.assertIn("core/audio/src/foo.cpp", body)
        self.assertIn("**Coverage**: 78%", body)

    def test_details_wraps_the_report(self) -> None:
        body = cdc.render(
            SAMPLE_REPORT,
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertIn("<details>", body)
        self.assertIn("</details>", body)

    def test_report_without_top_level_heading_is_preserved(self) -> None:
        report = "## Summary\n- **Coverage**: 91%\n"
        body = cdc.render(
            report,
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertIn(report.strip(), body)

    def test_empty_report_falls_back_gracefully(self) -> None:
        # A PR that doesn't touch instrumented source (docs-only, CI-only,
        # etc.) produces an empty markdown report. The comment must still
        # post cleanly — contributors expect a coverage note regardless.
        body = cdc.render(
            None,
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertIn(cdc.COMMENT_MARKER, body)
        self.assertIn("did not touch any instrumented source", body)
        self.assertNotIn("<details>", body)

    def test_whitespace_only_report_falls_back_gracefully(self) -> None:
        body = cdc.render(
            "   \n\n",
            flip_date="2026-05-05",
            threshold=75,
        )
        self.assertIn("did not touch any instrumented source", body)


class ReadReportTests(unittest.TestCase):
    def test_missing_file_returns_none(self) -> None:
        missing = pathlib.Path("/nonexistent/path/definitely-not-here.md")
        self.assertIsNone(cdc._read_report(missing))

    def test_reads_existing_file_verbatim(self) -> None:
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".md", delete=False, encoding="utf-8"
        ) as handle:
            handle.write(SAMPLE_REPORT)
            path = pathlib.Path(handle.name)
        try:
            self.assertEqual(cdc._read_report(path), SAMPLE_REPORT)
        finally:
            path.unlink()


class MainCliTests(unittest.TestCase):
    def test_main_writes_output_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            report = pathlib.Path(tmpdir) / "report.md"
            report.write_text(SAMPLE_REPORT, encoding="utf-8")
            out = pathlib.Path(tmpdir) / "comment.md"
            rc = cdc.main(
                [
                    "--report",
                    str(report),
                    "--flip-date",
                    "2026-05-05",
                    "--threshold",
                    "75",
                    "--out",
                    str(out),
                ]
            )
            self.assertEqual(rc, 0)
            body = out.read_text(encoding="utf-8")
            self.assertIn(cdc.COMMENT_MARKER, body)
            self.assertIn("core/audio/src/foo.cpp", body)

    def test_main_handles_missing_report(self) -> None:
        # The workflow step uses `continue-on-error: true` on diff-cover,
        # so the report file can be absent. We still want an informative
        # comment — never a silent failure.
        with tempfile.TemporaryDirectory() as tmpdir:
            out = pathlib.Path(tmpdir) / "comment.md"
            rc = cdc.main(
                [
                    "--report",
                    str(pathlib.Path(tmpdir) / "missing.md"),
                    "--flip-date",
                    "2026-05-05",
                    "--threshold",
                    "75",
                    "--out",
                    str(out),
                ]
            )
            self.assertEqual(rc, 0)
            body = out.read_text(encoding="utf-8")
            self.assertIn(cdc.COMMENT_MARKER, body)
            self.assertIn("did not touch any instrumented source", body)

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            report = root / "report.md"
            report.write_text(SAMPLE_REPORT, encoding="utf-8")
            out = root / "comment.md"
            argv = [
                str(HERE / "coverage_diff_comment.py"),
                "--report", str(report),
                "--flip-date", "2026-05-05",
                "--threshold", "75",
                "--out", str(out),
            ]

            with mock.patch.object(sys, "argv", argv):
                with self.assertRaises(SystemExit) as cm:
                    runpy.run_path(str(HERE / "coverage_diff_comment.py"), run_name="__main__")

            self.assertEqual(cm.exception.code, 0)
            self.assertIn(cdc.COMMENT_MARKER, out.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
