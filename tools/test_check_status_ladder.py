#!/usr/bin/env python3
"""Tests for tools/check_status_ladder.py."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from textwrap import dedent
from unittest import mock


_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "check_status_ladder", _HERE / "check_status_ladder.py"
)
assert _SPEC and _SPEC.loader
csl = importlib.util.module_from_spec(_SPEC)
sys.modules["check_status_ladder"] = csl
_SPEC.loader.exec_module(csl)


SYNTHETIC_MATRIX = dedent(
    """\
    schema_version: 2
    capability_groups:
      group_a:
        validated_inline:
          status: "usable"
          notes: Validated in CI.
        validated_block:
          status: usable
          notes: |-
            Golden screenshot
            round trip.
        missing_evidence:
          status: usable
          notes: Needs proof.
        partial_without_evidence:
          status: partial
          notes: No proof required.
      platform_maturity:
        accessibility:
          macos:
            status: 'usable'
            notes: Platform-scoped entries pass.
      hyphen_group:
        nested-feature:
          status: usable
          notes: clap-validator pass.
    """
)


def _write(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


class WalkStatusTests(unittest.TestCase):
    def test_walk_statuses_captures_notes_and_platform_scope(self) -> None:
        rows = {
            path: (status, notes, platform_scoped)
            for path, status, notes, platform_scoped in csl.walk_statuses(
                SYNTHETIC_MATRIX
            )
        }

        self.assertEqual(
            rows["capability_groups.group_a.validated_inline"],
            ("usable", "Validated in CI.", False),
        )
        self.assertEqual(
            rows["capability_groups.group_a.validated_block"],
            ("usable", "Golden screenshot round trip.", False),
        )
        self.assertEqual(
            rows["capability_groups.group_a.partial_without_evidence"],
            ("partial", "No proof required.", False),
        )
        self.assertEqual(
            rows["capability_groups.platform_maturity.accessibility.macos"],
            ("usable", "Platform-scoped entries pass.", True),
        )
        self.assertEqual(
            rows["capability_groups.hyphen_group.nested-feature"],
            ("usable", "clap-validator pass.", False),
        )

    def test_walk_statuses_ignores_comments_and_non_key_lines(self) -> None:
        matrix = dedent(
            """\
            # comment
            bad list item
            root:
              feature:
                status: usable
                # comment before notes
                notes: tests exist
            """
        )

        self.assertEqual(
            list(csl.walk_statuses(matrix)),
            [("root.feature", "usable", "tests exist", False)],
        )


class WaiverTests(unittest.TestCase):
    def test_load_waivers_strips_comments_and_blanks(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            waiver_path = _write(
                Path(td) / "waivers.txt",
                dedent(
                    """\
                    # comment
                    capability_groups.group_a.missing_evidence

                    capability_groups.group_b.item # reason
                    """
                ),
            )

            with mock.patch.object(csl, "WAIVERS_PATH", waiver_path):
                self.assertEqual(
                    csl.load_waivers(),
                    {
                        "capability_groups.group_a.missing_evidence",
                        "capability_groups.group_b.item",
                    },
                )

    def test_load_waivers_returns_empty_set_when_file_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            with mock.patch.object(csl, "WAIVERS_PATH", Path(td) / "missing.txt"):
                self.assertEqual(csl.load_waivers(), set())


class MainTests(unittest.TestCase):
    def _run_main(
        self,
        matrix_text: str,
        *,
        waivers_text: str = "",
        mode: str = "warn",
        matrix_name: str = "support-matrix.yaml",
    ) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            matrix_path = _write(root / matrix_name, matrix_text)
            waivers_path = _write(root / "waivers.txt", waivers_text)
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(csl, "MATRIX_PATH", matrix_path), \
                 mock.patch.object(csl, "WAIVERS_PATH", waivers_path), \
                 mock.patch.object(sys, "argv", ["check_status_ladder.py", "--mode", mode]), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = csl.main()
            return rc, stdout.getvalue(), stderr.getvalue()

    def test_warn_mode_reports_violations_but_returns_zero(self) -> None:
        rc, stdout, stderr = self._run_main(SYNTHETIC_MATRIX)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("checked 5 `usable` entries", stdout)
        self.assertIn("0 waived, 1 violations (mode=warn)", stdout)
        self.assertIn("capability_groups.group_a.missing_evidence", stdout)

    def test_report_mode_returns_one_and_prints_guidance(self) -> None:
        rc, stdout, stderr = self._run_main(SYNTHETIC_MATRIX, mode="report")

        self.assertEqual(rc, 1)
        self.assertEqual(stderr, "")
        self.assertIn("1 violations (mode=report)", stdout)
        self.assertIn("To waive an entry", stdout)

    def test_waived_violation_is_counted_without_failing(self) -> None:
        rc, stdout, stderr = self._run_main(
            SYNTHETIC_MATRIX,
            waivers_text="capability_groups.group_a.missing_evidence\n",
            mode="report",
        )

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("1 waived, 0 violations (mode=report)", stdout)

    def test_missing_matrix_returns_input_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = Path(td) / "missing.yaml"
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(csl, "MATRIX_PATH", missing), \
                 mock.patch.object(sys, "argv", ["check_status_ladder.py"]), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = csl.main()

        self.assertEqual(rc, 2)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("Failed to read", stderr.getvalue())


class KeywordTests(unittest.TestCase):
    def test_validation_keyword_matcher_accepts_known_evidence_terms(self) -> None:
        samples = [
            "covered by tests",
            "validated in ci",
            "golden screenshot",
            "round trip",
            "pluginval pass",
            "clap-validator pass",
            "lv2lint pass",
        ]

        for sample in samples:
            with self.subTest(sample=sample):
                self.assertIsNotNone(csl.VALIDATION_KEYWORDS.search(sample))


if __name__ == "__main__":
    unittest.main()
