#!/usr/bin/env python3
"""Additional coverage-lane tests for tools/scripts/run_swift_coverage.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "run_swift_coverage.py"
spec = importlib.util.spec_from_file_location("run_swift_coverage", SCRIPT)
assert spec and spec.loader
rsc = importlib.util.module_from_spec(spec)
sys.modules["run_swift_coverage"] = rsc
spec.loader.exec_module(rsc)


class SubprocessHelperTests(unittest.TestCase):
    def test_run_delegates_to_subprocess_with_apple_cwd(self) -> None:
        completed = subprocess.CompletedProcess(["swift"], 0, stdout="ok\n")
        with mock.patch.object(rsc.subprocess, "run", return_value=completed) as run:
            self.assertIs(rsc._run(["swift", "--version"], capture_output=True), completed)

        run.assert_called_once_with(
            ["swift", "--version"],
            cwd=rsc.APPLE_DIR,
            check=True,
            text=True,
            capture_output=True,
        )

    def test_codecov_report_path_rejects_empty_stdout(self) -> None:
        with mock.patch.object(
            rsc,
            "_run",
            return_value=subprocess.CompletedProcess([], 0, stdout="\n"),
        ):
            with self.assertRaises(SystemExit) as raised:
                rsc._codecov_report_path()

        self.assertIn("did not print a Codecov JSON path", str(raised.exception))

    def test_codecov_report_path_strips_stdout(self) -> None:
        with mock.patch.object(
            rsc,
            "_run",
            return_value=subprocess.CompletedProcess([], 0, stdout="  /tmp/report.json\n"),
        ):
            self.assertEqual(rsc._codecov_report_path(), pathlib.Path("/tmp/report.json"))

    def test_profdata_path_requires_default_profdata_next_to_report(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report = pathlib.Path(td) / "coverage.json"
            with self.assertRaises(SystemExit) as raised:
                rsc._profdata_path(report)

            profdata = pathlib.Path(td) / "default.profdata"
            profdata.write_bytes(b"profile")

            self.assertEqual(rsc._profdata_path(report), profdata)

        self.assertIn("reported profdata does not exist", str(raised.exception))

    def test_test_binary_paths_finds_sorted_swiftpm_binaries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            codecov = root / ".build" / "debug" / "codecov" / "coverage.json"
            binary_b = root / ".build" / "debug" / "B.xctest" / "Contents" / "MacOS" / "B"
            binary_a = root / ".build" / "debug" / "A.xctest" / "Contents" / "MacOS" / "A"
            binary_b.parent.mkdir(parents=True)
            binary_a.parent.mkdir(parents=True)
            binary_b.write_text("b", encoding="utf-8")
            binary_a.write_text("a", encoding="utf-8")

            self.assertEqual(rsc._test_binary_paths(codecov), [binary_a, binary_b])

        with tempfile.TemporaryDirectory() as td:
            codecov = pathlib.Path(td) / ".build" / "debug" / "codecov" / "coverage.json"
            with self.assertRaises(SystemExit) as raised:
                rsc._test_binary_paths(codecov)

        self.assertIn("could not find any SwiftPM test binaries", str(raised.exception))

    def test_export_lcov_rejects_empty_output(self) -> None:
        with mock.patch.object(
            rsc,
            "_run",
            return_value=subprocess.CompletedProcess([], 0, stdout=" \n"),
        ):
            with self.assertRaises(SystemExit) as raised:
                rsc._export_lcov(pathlib.Path("default.profdata"), [pathlib.Path("Tests")])

        self.assertIn("llvm-cov export produced no LCOV output", str(raised.exception))

    def test_relative_repo_path_returns_none_for_paths_outside_repo(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            self.assertIsNone(rsc._relative_repo_path(str(pathlib.Path(td) / "outside.swift")))

    def test_source_entries_returns_empty_for_reports_without_data(self) -> None:
        self.assertEqual(rsc._source_entries({}), [])
        self.assertEqual(rsc._source_entries({"data": []}), [])


class MainTests(unittest.TestCase):
    def test_main_rejects_missing_apple_directory(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = pathlib.Path(td) / "apple"
            with mock.patch.object(rsc, "APPLE_DIR", missing):
                with self.assertRaises(SystemExit) as raised:
                    rsc.main()

        self.assertIn("missing Swift package directory", str(raised.exception))

    def test_main_stages_json_lcov_and_summary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            apple_dir = root / "apple"
            output_dir = root / "coverage"
            codecov = root / ".build" / "debug" / "codecov" / "coverage.json"
            profdata = codecov.with_name("default.profdata")
            binary = root / ".build" / "debug" / "Pulp.xctest" / "Contents" / "MacOS" / "Pulp"
            apple_dir.mkdir()
            codecov.parent.mkdir(parents=True)
            binary.parent.mkdir(parents=True)
            profdata.write_bytes(b"profile")
            binary.write_bytes(b"binary")
            source_path = rsc.REPO_ROOT / "apple" / "Sources" / "PulpSwift" / "Bridge.swift"
            codecov.write_text(
                json.dumps(
                    {
                        "data": [
                            {
                                "files": [
                                    {
                                        "filename": str(source_path),
                                        "summary": {
                                            "lines": {
                                                "covered": 7,
                                                "count": 10,
                                                "percent": 70.0,
                                            }
                                        },
                                    }
                                ]
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            lcov = f"SF:{source_path}\nDA:1,1\nend_of_record\n"
            calls = [
                subprocess.CompletedProcess(["swift"], 0, stdout=""),
                subprocess.CompletedProcess(["swift"], 0, stdout=f"{codecov}\n"),
                subprocess.CompletedProcess(["xcrun"], 0, stdout=lcov),
            ]
            stdout = io.StringIO()

            with mock.patch.object(rsc, "APPLE_DIR", apple_dir), \
                 mock.patch.object(rsc, "OUTPUT_DIR", output_dir), \
                 mock.patch.object(rsc, "JSON_FILE", output_dir / "coverage.apple.json"), \
                 mock.patch.object(rsc, "LCOV_FILE", output_dir / "coverage.apple.lcov"), \
                 mock.patch.object(rsc, "SUMMARY_FILE", output_dir / "summary.txt"), \
                 mock.patch.object(rsc, "_run", side_effect=calls) as run, \
                 contextlib.redirect_stdout(stdout):
                rc = rsc.main()

            self.assertEqual(rc, 0)
            self.assertIn("Swift apple coverage", stdout.getvalue())
            self.assertIn("Swift JSON:", stdout.getvalue())
            self.assertIn("SF:apple/Sources/PulpSwift/Bridge.swift", (output_dir / "coverage.apple.lcov").read_text(encoding="utf-8"))
            self.assertIn("70.00%     7/10", (output_dir / "summary.txt").read_text(encoding="utf-8"))
            self.assertEqual(run.call_count, 3)

    def test_main_rejects_missing_reported_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            apple_dir = root / "apple"
            output_dir = root / "coverage"
            apple_dir.mkdir()
            missing = root / ".build" / "debug" / "codecov" / "coverage.json"
            calls = [
                subprocess.CompletedProcess(["swift"], 0, stdout=""),
                subprocess.CompletedProcess(["swift"], 0, stdout=f"{missing}\n"),
            ]

            with mock.patch.object(rsc, "APPLE_DIR", apple_dir), \
                 mock.patch.object(rsc, "OUTPUT_DIR", output_dir), \
                 mock.patch.object(rsc, "_run", side_effect=calls):
                with self.assertRaises(SystemExit) as raised:
                    rsc.main()

        self.assertIn("reported Codecov JSON does not exist", str(raised.exception))

    def test_script_entrypoint_runs_with_fake_swift_tools(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            bin_dir = root / "bin"
            codecov = root / ".build" / "debug" / "codecov" / "coverage.json"
            profdata = codecov.with_name("default.profdata")
            binary = root / ".build" / "debug" / "Pulp.xctest" / "Contents" / "MacOS" / "Pulp"
            source_path = rsc.REPO_ROOT / "apple" / "Sources" / "PulpSwift" / "Entrypoint.swift"
            bin_dir.mkdir()
            codecov.parent.mkdir(parents=True)
            binary.parent.mkdir(parents=True)
            profdata.write_bytes(b"profile")
            binary.write_bytes(b"binary")
            codecov.write_text(
                json.dumps(
                    {
                        "data": [
                            {
                                "files": [
                                    {
                                        "filename": str(source_path),
                                        "summary": {
                                            "lines": {
                                                "covered": 1,
                                                "count": 1,
                                                "percent": 100.0,
                                            }
                                        },
                                    }
                                ]
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )

            swift = bin_dir / "swift"
            swift.write_text(
                "#!/bin/sh\n"
                "if [ \"$3\" = \"--show-codecov-path\" ]; then\n"
                f"  printf '%s\\n' '{codecov}'\n"
                "fi\n",
                encoding="utf-8",
            )
            xcrun = bin_dir / "xcrun"
            xcrun.write_text(
                "#!/bin/sh\n"
                f"printf '%s\\n' 'SF:{source_path}' 'DA:1,1' 'end_of_record'\n",
                encoding="utf-8",
            )
            swift.chmod(0o755)
            xcrun.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}{os.pathsep}{env['PATH']}"
            try:
                proc = subprocess.run(
                    [sys.executable, str(SCRIPT)],
                    cwd=rsc.REPO_ROOT,
                    check=True,
                    text=True,
                    capture_output=True,
                    env=env,
                )
            finally:
                with contextlib.suppress(FileNotFoundError):
                    rsc.LCOV_FILE.unlink()
                with contextlib.suppress(FileNotFoundError):
                    rsc.JSON_FILE.unlink()
                with contextlib.suppress(FileNotFoundError):
                    rsc.SUMMARY_FILE.unlink()

        self.assertIn("Swift apple coverage", proc.stdout)
        self.assertIn("Swift JSON:", proc.stdout)

    def test_script_entrypoint_runs_in_process_with_mocked_tools(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            codecov = root / ".build" / "debug" / "codecov" / "coverage.json"
            profdata = codecov.with_name("default.profdata")
            binary = root / ".build" / "debug" / "Pulp.xctest" / "Contents" / "MacOS" / "Pulp"
            source_path = rsc.REPO_ROOT / "apple" / "Sources" / "PulpSwift" / "Entrypoint.swift"
            codecov.parent.mkdir(parents=True)
            binary.parent.mkdir(parents=True)
            profdata.write_bytes(b"profile")
            binary.write_bytes(b"binary")
            codecov.write_text(
                json.dumps(
                    {
                        "data": [
                            {
                                "files": [
                                    {
                                        "filename": str(source_path),
                                        "summary": {
                                            "lines": {
                                                "covered": 1,
                                                "count": 1,
                                                "percent": 100.0,
                                            }
                                        },
                                    }
                                ]
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            lcov = f"SF:{source_path}\nDA:1,1\nend_of_record\n"
            calls = [
                subprocess.CompletedProcess(["swift"], 0, stdout=""),
                subprocess.CompletedProcess(["swift"], 0, stdout=f"{codecov}\n"),
                subprocess.CompletedProcess(["xcrun"], 0, stdout=lcov),
            ]
            stdout = io.StringIO()

            try:
                with mock.patch.object(subprocess, "run", side_effect=calls), \
                     contextlib.redirect_stdout(stdout):
                    with self.assertRaises(SystemExit) as raised:
                        runpy.run_path(str(SCRIPT), run_name="__main__")
            finally:
                with contextlib.suppress(FileNotFoundError):
                    rsc.LCOV_FILE.unlink()
                with contextlib.suppress(FileNotFoundError):
                    rsc.JSON_FILE.unlink()
                with contextlib.suppress(FileNotFoundError):
                    rsc.SUMMARY_FILE.unlink()

        self.assertEqual(raised.exception.code, 0)
        self.assertIn("Swift apple coverage", stdout.getvalue())
        self.assertIn("Swift JSON:", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
