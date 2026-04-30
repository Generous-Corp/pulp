#!/usr/bin/env python3
"""Extra focused coverage for docs_sync_check.py.

Keeps git interactions mocked so the tests stay deterministic in agent
worktrees and CI merge checkouts.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import docs_sync_check as dsc


DOCS = [
    dsc.DocEntry(
        name="coverage.md",
        path="docs/guides/coverage.md",
        paths=("codecov.yml", "scripts/run_coverage.sh"),
    ),
]


class GitHelperTests(unittest.TestCase):

    def test_repo_root_uses_git_rev_parse(self) -> None:
        with mock.patch.object(
            dsc.subprocess,
            "check_output",
            return_value="/tmp/pulp\n",
        ) as check_output:
            self.assertEqual(dsc.repo_root(), Path("/tmp/pulp"))

        check_output.assert_called_once_with(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
        )

    def test_diff_files_strips_blank_lines(self) -> None:
        with mock.patch.object(
            dsc.subprocess,
            "check_output",
            return_value="codecov.yml\n\n docs/guides/coverage.md \n",
        ) as check_output:
            self.assertEqual(
                dsc.diff_files("origin/main"),
                ["codecov.yml", "docs/guides/coverage.md"],
            )

        check_output.assert_called_once_with(
            ["git", "diff", "--name-only", "origin/main...HEAD"],
            text=True,
        )

    def test_range_commit_messages_returns_git_log_output(self) -> None:
        with mock.patch.object(
            dsc.subprocess,
            "check_output",
            return_value="fix: docs\n\nDocs-Update: skip doc=x reason=\"y\"\n",
        ) as check_output:
            self.assertIn("Docs-Update", dsc.range_commit_messages("main"))

        check_output.assert_called_once_with(
            ["git", "log", "--format=%B", "main..HEAD"],
            text=True,
        )

    def test_range_commit_messages_returns_empty_on_git_log_failure(self) -> None:
        with mock.patch.object(
            dsc.subprocess,
            "check_output",
            side_effect=subprocess.CalledProcessError(128, ["git", "log"]),
        ):
            self.assertEqual(dsc.range_commit_messages("missing-base"), "")


class LoadMapTests(unittest.TestCase):

    def test_load_map_converts_entries_to_doc_entries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = Path(td) / "docs_sync_map.json"
            config.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "docs": {
                            "coverage.md": {
                                "path": "docs/guides/coverage.md",
                                "paths": ["codecov.yml", 42],
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )

            docs = dsc.load_map(config)

        self.assertEqual(
            docs,
            [
                dsc.DocEntry(
                    name="coverage.md",
                    path="docs/guides/coverage.md",
                    paths=("codecov.yml", "42"),
                )
            ],
        )

    def test_load_map_rejects_unsupported_schema_version(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = Path(td) / "docs_sync_map.json"
            config.write_text(
                json.dumps({"schema_version": 2, "docs": {}}),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "unsupported schema_version"):
                dsc.load_map(config)


class MatchEdgeTests(unittest.TestCase):

    def test_starstar_prefix_fallback_matches_children(self) -> None:
        with mock.patch.object(dsc.fnmatch, "fnmatch", return_value=False):
            self.assertTrue(
                dsc.matches_any(
                    "core/audio/src/foo.cpp",
                    ("core/audio/**",),
                )
            )


class RenderEdgeTests(unittest.TestCase):

    def test_render_no_findings_reports_noop(self) -> None:
        report, ok = dsc.render([])
        self.assertTrue(ok)
        self.assertIn("no mapped paths touched", report)

    def test_render_truncates_long_touched_path_list(self) -> None:
        touched = [f"tools/cli/file_{i}.cpp" for i in range(7)]
        report, ok = dsc.render(
            [
                dsc.Finding(
                    doc=DOCS[0],
                    touched_paths=touched,
                    doc_modified=False,
                    bypass_reason=None,
                )
            ]
        )

        self.assertFalse(ok)
        self.assertIn("file_4.cpp", report)
        self.assertNotIn("file_5.cpp", report)
        self.assertIn("... +2 more", report)


class MainTests(unittest.TestCase):

    def run_main(
        self,
        argv: list[str],
        *,
        diff: list[str] | None = None,
        message: str = "",
        diff_error: subprocess.CalledProcessError | None = None,
    ) -> tuple[int, str, str]:
        old_cwd = os.getcwd()
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            diff_mock = mock.Mock()
            if diff_error is not None:
                diff_mock.side_effect = diff_error
            else:
                diff_mock.return_value = diff or []

            try:
                with mock.patch.object(dsc, "repo_root", return_value=Path(td)), \
                     mock.patch.object(dsc, "load_map", return_value=DOCS), \
                     mock.patch.object(dsc, "diff_files", diff_mock), \
                     mock.patch.object(
                         dsc,
                         "range_commit_messages",
                         return_value=message,
                     ), \
                     contextlib.redirect_stdout(stdout), \
                     contextlib.redirect_stderr(stderr):
                    code = dsc.main(argv)
            finally:
                os.chdir(old_cwd)

        return code, stdout.getvalue(), stderr.getvalue()

    def test_main_report_mode_fails_when_doc_is_missing(self) -> None:
        code, stdout, stderr = self.run_main(
            ["--base", "main", "--mode", "report"],
            diff=["codecov.yml"],
        )

        self.assertEqual(code, 1)
        self.assertIn("NOT updated", stdout)
        self.assertIn("Docs-sync check FAILED.", stderr)
        self.assertIn("Docs-Update: skip", stderr)

    def test_main_hint_mode_reports_but_exits_zero(self) -> None:
        code, stdout, stderr = self.run_main(
            ["--mode", "hint"],
            diff=["codecov.yml"],
        )

        self.assertEqual(code, 0)
        self.assertIn("NOT updated", stdout)
        self.assertEqual(stderr, "")

    def test_main_diff_failure_returns_two_with_fetch_hint(self) -> None:
        error = subprocess.CalledProcessError(128, ["git", "diff"])
        code, stdout, stderr = self.run_main(
            ["--base", "missing-base"],
            diff_error=error,
        )

        self.assertEqual(code, 2)
        self.assertEqual(stdout, "")
        self.assertIn("missing-base", stderr)
        self.assertIn("Fetch the base ref", stderr)

    def test_script_entrypoint_exits_with_main_result(self) -> None:
        def check_output(cmd: list[str], *, text: bool) -> str:
            self.assertTrue(text)
            if cmd == ["git", "rev-parse", "--show-toplevel"]:
                return str(Path.cwd()) + "\n"
            if cmd == ["git", "diff", "--name-only", "origin/main...HEAD"]:
                return ""
            if cmd == ["git", "log", "--format=%B", "origin/main..HEAD"]:
                return ""
            raise AssertionError(f"unexpected command: {cmd!r}")

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(subprocess, "check_output", side_effect=check_output), \
             mock.patch.object(sys, "argv", [str(Path(dsc.__file__))]), \
             contextlib.redirect_stdout(stdout), \
             contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as exit_ctx:
                runpy.run_path(str(Path(dsc.__file__)), run_name="__main__")

        self.assertEqual(exit_ctx.exception.code, 0)
        self.assertIn("no mapped paths touched", stdout.getvalue())
        self.assertEqual(stderr.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
