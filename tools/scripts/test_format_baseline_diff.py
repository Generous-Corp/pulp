#!/usr/bin/env python3
"""Coverage-lane tests for tools/scripts/format_baseline_diff.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parent / "format_baseline_diff.py"
spec = importlib.util.spec_from_file_location("format_baseline_diff", SCRIPT)
assert spec and spec.loader
fbd = importlib.util.module_from_spec(spec)
sys.modules["format_baseline_diff"] = fbd
spec.loader.exec_module(fbd)


class FormatBaselineDiffTests(unittest.TestCase):
    def _root(self) -> tempfile.TemporaryDirectory[str]:
        return tempfile.TemporaryDirectory(prefix="pulp-format-baseline-test-")

    def _prepare_root(self, root: pathlib.Path) -> pathlib.Path:
        baseline = root / "test" / "fixtures" / "format-baseline"
        baseline.mkdir(parents=True)
        capture = root / "tools" / "scripts" / "format_baseline_capture.sh"
        capture.parent.mkdir(parents=True, exist_ok=True)
        capture.write_text("#!/bin/sh\n", encoding="utf-8")
        return baseline

    def _run(
        self,
        root: pathlib.Path,
        run_result: int = 0,
        captured: dict[str, str] | None = None,
        extra_args: list[str] | None = None,
    ) -> tuple[int, str]:
        captured = captured or {}
        stderr = io.StringIO()

        def fake_check_output(cmd: list[str]) -> bytes:
            self.assertEqual(cmd, ["git", "rev-parse", "--show-toplevel"])
            return f"{root}\n".encode()

        def fake_run(cmd: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
            self.assertEqual(cwd, root)
            output_dir = pathlib.Path(cmd[cmd.index("--output") + 1])
            for name, text in captured.items():
                (output_dir / name).write_text(text, encoding="utf-8")
            return subprocess.CompletedProcess(cmd, run_result)

        with mock.patch.object(fbd.subprocess, "check_output", side_effect=fake_check_output), \
             mock.patch.object(fbd.subprocess, "run", side_effect=fake_run), \
             contextlib.redirect_stderr(stderr):
            rc = fbd.main(extra_args or [])
        return rc, stderr.getvalue()

    def test_missing_baseline_directory_is_a_hard_failure(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            capture = root / "tools" / "scripts" / "format_baseline_capture.sh"
            capture.parent.mkdir(parents=True)
            capture.write_text("#!/bin/sh\n", encoding="utf-8")

            rc, stderr = self._run(root)

        self.assertEqual(rc, 1)
        self.assertIn("No baseline directory", stderr)

    def test_missing_capture_script_is_a_hard_failure(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            (root / "test" / "fixtures" / "format-baseline").mkdir(parents=True)

            rc, stderr = self._run(root)

        self.assertEqual(rc, 1)
        self.assertIn("Capture script missing", stderr)

    def test_capture_skip_and_capture_failure_return_distinct_codes(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            self._prepare_root(root)

            skipped, skipped_err = self._run(root, run_result=2)
            failed, failed_err = self._run(root, run_result=7)

        self.assertEqual(skipped, 2)
        self.assertIn("No validators available", skipped_err)
        self.assertEqual(failed, 7)
        self.assertIn("Capture script exited 7", failed_err)

    def test_empty_capture_is_treated_as_missing_signal(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            self._prepare_root(root)

            rc, stderr = self._run(root, captured={})

        self.assertEqual(rc, 2)
        self.assertIn("Capture produced no files", stderr)

    def test_missing_committed_baseline_bootstraps_without_failure(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            self._prepare_root(root)

            rc, stderr = self._run(
                root,
                captured={"clap-validator.txt": "\n".join(f"line {i}" for i in range(35))},
            )

        self.assertEqual(rc, 0)
        self.assertIn("No committed baseline for clap-validator.txt yet", stderr)
        self.assertIn("First ~30 lines", stderr)
        self.assertIn("line 29", stderr)
        self.assertNotIn("line 30", stderr)
        self.assertIn("OK (bootstrap)", stderr)

    def test_matching_capture_reports_success(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "auval.txt").write_text("same\noutput\n", encoding="utf-8")

            rc, stderr = self._run(root, captured={"auval.txt": "same\noutput\n"})

        self.assertEqual(rc, 0)
        self.assertIn("OK — all 1 validator output(s) match", stderr)

    def test_diff_against_existing_baseline_is_truncated_and_blocks(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "pluginval.txt").write_text("old a\nold b\nold c\n", encoding="utf-8")

            rc, stderr = self._run(
                root,
                captured={"pluginval.txt": "new a\nnew b\nnew c\nnew d\n"},
                extra_args=["--max-diff-lines", "4"],
            )

        self.assertEqual(rc, 1)
        self.assertIn("DIFF in pluginval.txt", stderr)
        self.assertIn("--- baseline/pluginval.txt", stderr)
        self.assertIn("... ", stderr)
        self.assertIn("BLOCKED: 1 diff(s)", stderr)


if __name__ == "__main__":
    unittest.main()
