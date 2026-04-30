#!/usr/bin/env python3
"""Extra focused tests for coverage_tier_check.py."""

from __future__ import annotations

import pathlib
import runpy
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

import coverage_tier_check as ctc


class LoadTargetsExtraTests(unittest.TestCase):

    def _write_targets(self, body: str) -> pathlib.Path:
        tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(tmpdir.cleanup)
        path = pathlib.Path(tmpdir.name) / "coverage-targets.yaml"
        path.write_text(textwrap.dedent(body).lstrip(), encoding="utf-8")
        return path

    def test_load_targets_coerces_config_values(self) -> None:
        path = self._write_targets(
            """
            version: "1"
            tiers:
              - name: 123
                line_target: "70"
                paths:
                  - core/audio/**
                  - 456
            """
        )

        tiers = ctc.load_targets(path)

        self.assertEqual(
            tiers,
            [
                ctc.Tier(
                    name="123",
                    line_target=70,
                    paths=("core/audio/**", "456"),
                )
            ],
        )

    def test_load_targets_rejects_missing_tiers(self) -> None:
        path = self._write_targets("version: 1\n")

        with self.assertRaisesRegex(ValueError, "expected top-level 'tiers' list"):
            ctc.load_targets(path)

    def test_load_targets_rejects_unknown_version(self) -> None:
        path = self._write_targets(
            """
            version: 2
            tiers: []
            """
        )

        with self.assertRaisesRegex(ValueError, "unsupported version 2"):
            ctc.load_targets(path)


class ClassificationExtraTests(unittest.TestCase):

    def test_classify_file_uses_fnmatch_when_prefix_match_does_not_apply(self) -> None:
        tier = ctc.Tier("docs", 50, ("*.md",))

        self.assertEqual(ctc.classify_file("README.md", [tier]), tier)


class CoberturaParsingExtraTests(unittest.TestCase):

    def _write_xml(self, body: str) -> pathlib.Path:
        tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(tmpdir.cleanup)
        path = pathlib.Path(tmpdir.name) / "coverage.xml"
        path.write_text(textwrap.dedent(body).lstrip(), encoding="utf-8")
        return path

    def test_parse_cobertura_skips_incomplete_or_malformed_lines(self) -> None:
        xml_path = self._write_xml(
            """
            <coverage>
              <packages>
                <package>
                  <classes>
                    <class filename="core/audio/src/foo.cpp">
                      <lines>
                        <line number="10" hits="3" />
                        <line number="11" />
                        <line number="bad" hits="9" />
                        <line hits="4" />
                        <line number="12" hits="bad" />
                      </lines>
                    </class>
                    <class>
                      <lines>
                        <line number="1" hits="1" />
                      </lines>
                    </class>
                  </classes>
                </package>
              </packages>
            </coverage>
            """
        )

        coverage = ctc.parse_cobertura(xml_path)

        self.assertEqual(list(coverage), ["core/audio/src/foo.cpp"])
        self.assertEqual(coverage["core/audio/src/foo.cpp"].hits, {10: 3, 11: 0})


class DiffDiscoveryExtraTests(unittest.TestCase):

    @mock.patch("coverage_tier_check.subprocess.check_output")
    def test_diff_files_strips_blank_lines(self, check_output: mock.Mock) -> None:
        check_output.return_value = "\ncore/audio/src/a.cpp\n\n tools/cli/cmd.cpp \n"

        files = ctc.diff_files("origin/main")

        self.assertEqual(files, ["core/audio/src/a.cpp", "tools/cli/cmd.cpp"])
        check_output.assert_called_once_with(
            ["git", "diff", "--name-only", "origin/main...HEAD"],
            text=True,
        )

    @mock.patch("coverage_tier_check.subprocess.check_output")
    def test_diff_files_wraps_git_failure(self, check_output: mock.Mock) -> None:
        check_output.side_effect = subprocess.CalledProcessError(
            128,
            ["git", "diff"],
        )

        with self.assertRaisesRegex(ValueError, "git diff failed"):
            ctc.diff_files("origin/main")

    @mock.patch("coverage_tier_check.subprocess.check_output")
    def test_diff_lines_parses_ranges_single_lines_and_ignores_bad_hunks(
        self,
        check_output: mock.Mock,
    ) -> None:
        check_output.return_value = "\n".join(
            [
                "diff --git a/core/audio/src/a.cpp b/core/audio/src/a.cpp",
                "@@ -1,2 +10,3 @@",
                "@@ -5 +20 @@",
                "@@ malformed @@",
                "@@ -9 +bad,2 @@",
            ]
        )

        changed = ctc.diff_lines("origin/main", "core/audio/src/a.cpp")

        self.assertEqual(changed, {10, 11, 12, 20})
        check_output.assert_called_once_with(
            [
                "git",
                "diff",
                "--unified=0",
                "origin/main...HEAD",
                "--",
                "core/audio/src/a.cpp",
            ],
            text=True,
        )

    @mock.patch("coverage_tier_check.subprocess.check_output")
    def test_diff_lines_returns_empty_set_on_git_failure(
        self,
        check_output: mock.Mock,
    ) -> None:
        check_output.side_effect = subprocess.CalledProcessError(
            128,
            ["git", "diff"],
        )

        self.assertEqual(ctc.diff_lines("origin/main", "missing.cpp"), set())


class MainExtraTests(unittest.TestCase):

    def _tmp_path(self, name: str, body: str | None = "") -> pathlib.Path:
        tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(tmpdir.cleanup)
        path = pathlib.Path(tmpdir.name) / name
        if body is not None:
            path.write_text(body, encoding="utf-8")
        return path

    def test_main_skips_missing_cobertura(self) -> None:
        report = self._tmp_path("report.md")
        cobertura = report.parent / "missing.xml"
        targets = self._tmp_path("targets.yaml", "version: 1\ntiers: []\n")

        rc = ctc.main(
            [
                "--cobertura",
                str(cobertura),
                "--targets",
                str(targets),
                "--compare-branch",
                "origin/main",
                "--markdown-report",
                str(report),
            ]
        )

        self.assertEqual(rc, 0)
        self.assertIn("per-tier gate skipped", report.read_text(encoding="utf-8"))

    def test_main_returns_success_and_writes_rendered_report(self) -> None:
        cobertura = self._tmp_path("coverage.xml", "<coverage />\n")
        targets = self._tmp_path("targets.yaml", "version: 1\ntiers: []\n")
        report = self._tmp_path("report.md")
        tier = ctc.Tier("audio-critical", 80, ("core/audio/**",))

        with (
            mock.patch("coverage_tier_check.load_targets", return_value=[tier]),
            mock.patch("coverage_tier_check.parse_cobertura", return_value={}),
            mock.patch(
                "coverage_tier_check.diff_files",
                return_value=["core/audio/src/foo.cpp"],
            ),
            mock.patch(
                "coverage_tier_check.diff_lines",
                return_value=set(),
            ) as diff_lines,
            mock.patch("builtins.print") as mocked_print,
        ):
            rc = ctc.main(
                [
                    "--cobertura",
                    str(cobertura),
                    "--targets",
                    str(targets),
                    "--compare-branch",
                    "origin/main",
                    "--markdown-report",
                    str(report),
                ]
            )

        self.assertEqual(rc, 0)
        body = report.read_text(encoding="utf-8")
        self.assertIn("All touched tiers meet their per-tier floors.", body)
        diff_lines.assert_called_once_with("origin/main", "core/audio/src/foo.cpp")
        mocked_print.assert_called_once_with(body)

    def test_main_returns_failure_when_rendered_results_fail(self) -> None:
        cobertura = self._tmp_path("coverage.xml", "<coverage />\n")
        targets = self._tmp_path("targets.yaml", "version: 1\ntiers: []\n")
        report = self._tmp_path("report.md")
        tier = ctc.Tier("audio-critical", 80, ("core/audio/**",))
        coverage = {
            "core/audio/src/foo.cpp": ctc.FileCoverage(
                path="core/audio/src/foo.cpp",
                hits={10: 0, 11: 1},
            )
        }

        with (
            mock.patch("coverage_tier_check.load_targets", return_value=[tier]),
            mock.patch("coverage_tier_check.parse_cobertura", return_value=coverage),
            mock.patch(
                "coverage_tier_check.diff_files",
                return_value=["core/audio/src/foo.cpp"],
            ),
            mock.patch("coverage_tier_check.diff_lines", return_value={10, 11}),
            mock.patch("builtins.print"),
        ):
            rc = ctc.main(
                [
                    "--cobertura",
                    str(cobertura),
                    "--targets",
                    str(targets),
                    "--compare-branch",
                    "origin/main",
                    "--markdown-report",
                    str(report),
                ]
            )

        self.assertEqual(rc, 1)
        self.assertIn("Per-tier gate failed", report.read_text(encoding="utf-8"))

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        report = self._tmp_path("report.md")
        missing_cobertura = report.parent / "missing.xml"
        targets = self._tmp_path("targets.yaml", "version: 1\ntiers: []\n")
        script = pathlib.Path(__file__).with_name("coverage_tier_check.py")

        with mock.patch.object(
            sys,
            "argv",
            [
                str(script),
                "--cobertura",
                str(missing_cobertura),
                "--targets",
                str(targets),
                "--compare-branch",
                "origin/main",
                "--markdown-report",
                str(report),
            ],
        ):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(script), run_name="__main__")

        self.assertEqual(cm.exception.code, 0)
        self.assertIn("per-tier gate skipped", report.read_text(encoding="utf-8"))


class RenderExtraTests(unittest.TestCase):

    def test_render_empty_results_is_successful_summary(self) -> None:
        body = ctc.render([])

        self.assertIn("## Per-tier diff coverage", body)
        self.assertIn("All touched tiers meet their per-tier floors.", body)

    def test_zero_touched_line_result_has_perfect_percent(self) -> None:
        result = ctc.TierResult(tier=ctc.Tier("infra", 50, ("tools/**",)))

        self.assertEqual(result.percent, 100.0)


class AggregateExtraTests(unittest.TestCase):

    def test_covered_file_with_no_changed_lines_does_not_record_file(self) -> None:
        tier = ctc.Tier("audio-critical", 80, ("core/audio/**",))
        coverage = {
            "core/audio/src/foo.cpp": ctc.FileCoverage(
                path="core/audio/src/foo.cpp",
                hits={10: 1},
            )
        }

        results = ctc.aggregate(
            [tier],
            ["core/audio/src/foo.cpp"],
            coverage,
            lines_getter=lambda _p: set(),
        )

        self.assertEqual(results[0].touched_lines, 0)
        self.assertEqual(results[0].files, [])


class InstrumentedSourceExtraTests(unittest.TestCase):

    def test_extensionless_path_is_not_instrumented(self) -> None:
        self.assertFalse(ctc.is_instrumented_source("tools/scripts/pulp-helper"))


if __name__ == "__main__":
    unittest.main()
