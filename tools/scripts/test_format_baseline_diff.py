#!/usr/bin/env python3
"""Coverage-lane tests for tools/scripts/format_baseline_diff.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import runpy
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

    def test_diff_under_limit_is_not_truncated(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "auval.txt").write_text("old\n", encoding="utf-8")

            rc, stderr = self._run(
                root,
                captured={"auval.txt": "new\n"},
                extra_args=["--max-diff-lines", "99"],
            )

        self.assertEqual(rc, 1)
        self.assertNotIn("more diff lines truncated", stderr)

    def test_multiple_diffs_are_counted(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "auval.txt").write_text("old\n", encoding="utf-8")
            (baseline / "clap.txt").write_text("old\n", encoding="utf-8")

            rc, stderr = self._run(
                root,
                captured={"auval.txt": "new\n", "clap.txt": "new\n"},
            )

        self.assertEqual(rc, 1)
        self.assertIn("BLOCKED: 2 diff(s)", stderr)

    def test_mixed_match_and_missing_baseline_bootstraps(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "auval.txt").write_text("same\n", encoding="utf-8")

            rc, stderr = self._run(
                root,
                captured={"auval.txt": "same\n", "new-validator.txt": "fresh\n"},
            )

        self.assertEqual(rc, 0)
        self.assertIn("No committed baseline for new-validator.txt", stderr)
        self.assertIn("OK (bootstrap): 1 validator output", stderr)

    def test_custom_plugin_and_baseline_dir_are_forwarded(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = root / "custom" / "baselines"
            baseline.mkdir(parents=True)
            (baseline / "validator.txt").write_text("same\n", encoding="utf-8")
            capture = root / "tools" / "scripts" / "format_baseline_capture.sh"
            capture.parent.mkdir(parents=True, exist_ok=True)
            capture.write_text("#!/bin/sh\n", encoding="utf-8")
            seen_cmd: list[str] = []

            def fake_check_output(cmd: list[str]) -> bytes:
                return f"{root}\n".encode()

            def fake_run(cmd: list[str], cwd: pathlib.Path):
                seen_cmd[:] = cmd
                output_dir = pathlib.Path(cmd[cmd.index("--output") + 1])
                (output_dir / "validator.txt").write_text("same\n", encoding="utf-8")
                return subprocess.CompletedProcess(cmd, 0)

            with mock.patch.object(fbd.subprocess, "check_output", side_effect=fake_check_output), \
                 mock.patch.object(fbd.subprocess, "run", side_effect=fake_run), \
                 contextlib.redirect_stderr(io.StringIO()):
                rc = fbd.main([
                    "--plugin", "PulpSynth",
                    "--baseline-dir", "custom/baselines",
                ])

        self.assertEqual(rc, 0)
        self.assertIn("PulpSynth", seen_cmd)

    def test_git_root_failure_propagates(self) -> None:
        with mock.patch.object(
            fbd.subprocess,
            "check_output",
            side_effect=subprocess.CalledProcessError(128, ["git"]),
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                fbd.main([])

    def test_workflow_refreshes_au_registry_after_install_and_cleanup(self) -> None:
        workflow = (
            SCRIPT.parents[2] / ".github" / "workflows" / "format-baseline-diff.yml"
        ).read_text(encoding="utf-8")
        install = workflow.split(
            "- name: Install plugin bundles to system folders + ad-hoc codesign", 1
        )[1].split("- name: Install clap-validator", 1)[0]
        cleanup = workflow.split("- name: Clean up installed plugin bundles", 1)[1]

        self.assertIn("killall -9 AudioComponentRegistrar", install)
        self.assertIn("sleep 5", install)
        self.assertIn("killall -9 AudioComponentRegistrar", cleanup)

    def test_script_entrypoint_success(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "auval.txt").write_text("same\n", encoding="utf-8")

            def fake_check_output(cmd: list[str]) -> bytes:
                return f"{root}\n".encode()

            def fake_run(cmd: list[str], cwd: pathlib.Path):
                output_dir = pathlib.Path(cmd[cmd.index("--output") + 1])
                (output_dir / "auval.txt").write_text("same\n", encoding="utf-8")
                return subprocess.CompletedProcess(cmd, 0)

            with mock.patch.object(sys, "argv", [str(SCRIPT)]), \
                 mock.patch.object(fbd.subprocess, "check_output", side_effect=fake_check_output), \
                 mock.patch.object(fbd.subprocess, "run", side_effect=fake_run), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 0)


    # ── --diag-dir: the captured output must outlive the temp dir ──────
    def test_diag_dir_keeps_captured_output_when_capture_fails(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            self._prepare_root(root)
            diag = root / "diag"

            rc, stderr = self._run(
                root,
                run_result=1,
                captured={"PulpEffect.clap.txt": "ERROR: could not load bundle\n"},
                extra_args=["--diag-dir", str(diag)],
            )

            kept = diag / "captured" / "PulpEffect.clap.txt"
            self.assertTrue(kept.is_file(),
                            "captured output must survive the temp dir")
            self.assertIn("ERROR: could not load bundle", kept.read_text())

        self.assertEqual(rc, 1)
        self.assertIn("diagnostics written to", stderr)
        self.assertIn("PulpEffect.clap.txt", stderr)

    def test_diag_dir_keeps_captured_output_on_the_success_path(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            baseline = self._prepare_root(root)
            (baseline / "PulpEffect.clap.txt").write_text("ok\n", encoding="utf-8")
            diag = root / "diag"

            rc, _ = self._run(
                root,
                run_result=0,
                captured={"PulpEffect.clap.txt": "ok\n"},
                extra_args=["--diag-dir", str(diag)],
            )

            self.assertTrue((diag / "captured" / "PulpEffect.clap.txt").is_file())

        self.assertEqual(rc, 0)

    def test_diag_dir_is_forwarded_to_the_capture_script(self) -> None:
        seen: list[list[str]] = []

        with self._root() as td:
            root = pathlib.Path(td)
            self._prepare_root(root)
            diag = root / "diag"

            def fake_check_output(cmd: list[str]) -> bytes:
                return f"{root}\n".encode()

            def fake_run(cmd: list[str], cwd: pathlib.Path):
                seen.append(cmd)
                return subprocess.CompletedProcess(cmd, 2)

            with mock.patch.object(fbd.subprocess, "check_output",
                                   side_effect=fake_check_output), \
                 mock.patch.object(fbd.subprocess, "run", side_effect=fake_run), \
                 contextlib.redirect_stderr(io.StringIO()):
                fbd.main(["--diag-dir", str(diag)])

        self.assertEqual(len(seen), 1)
        self.assertIn("--diag-dir", seen[0])
        # The script resolves the path before forwarding it, so the capture
        # script gets an absolute dir regardless of the caller's cwd.
        self.assertEqual(seen[0][seen[0].index("--diag-dir") + 1],
                         str(diag.resolve()))

    def test_capture_failure_without_diag_dir_says_output_was_discarded(self) -> None:
        with self._root() as td:
            root = pathlib.Path(td)
            self._prepare_root(root)

            rc, stderr = self._run(
                root,
                run_result=1,
                captured={"PulpEffect.clap.txt": "ERROR\n"},
            )

        self.assertEqual(rc, 1)
        self.assertIn("--diag-dir", stderr)


if __name__ == "__main__":
    unittest.main()
