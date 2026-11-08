#!/usr/bin/env python3
"""Tests for the dependency request recorder and its checker.

The checker cases drive dependency_request_check.py on hand-written logs. The
recorder cases configure a tiny CMake project with the recorder injected and
assert the log it writes, so they prove what the recorder sees and that it
leaves CMake's own handling untouched. They need CMake 3.24 or newer and are
skipped, saying so, when it is missing.
"""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
RECORDER = REPO / "tools/ci/dependency_request_recorder.cmake"
sys.path.insert(0, str(HERE))

import dependency_request_check as drc  # noqa: E402


def write_log(directory: Path, entries: list[dict[str, str]] | str) -> Path:
    path = directory / "dependency-requests.jsonl"
    if isinstance(entries, str):
        path.write_text(entries, encoding="utf-8")
    else:
        path.write_text(
            "".join(json.dumps(e) + "\n" for e in entries), encoding="utf-8"
        )
    return path


def run_checker(args: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = drc.main(args)
    return code, out.getvalue(), err.getvalue()


class CheckerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-dep-requests-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def log(self, *names: str) -> Path:
        return write_log(
            self.tmp, [{"method": "FIND_PACKAGE", "name": n} for n in names]
        )

    def test_a_clean_log_passes_and_prints_every_request(self) -> None:
        path = self.log("Threads", "Git")
        code, out, _ = run_checker(["--log", str(path), "--forbid", "skia"])
        self.assertEqual(code, 0)
        self.assertIn("2 request(s)", out)
        self.assertIn("Threads", out)
        self.assertIn("Git", out)
        self.assertIn("OK (forbid skia)", out)

    def test_a_forbidden_request_fails_case_insensitively(self) -> None:
        path = self.log("Threads", "Skia", "VellumGpuRuntime")
        code, _, err = run_checker(
            ["--log", str(path), "--forbid", "skia", "--forbid", "*vellum*"]
        )
        self.assertEqual(code, 1)
        self.assertIn("forbidden 'skia' was requested: Skia", err)
        self.assertIn("forbidden '*vellum*' was requested: VellumGpuRuntime", err)

    def test_a_glob_matches_the_whole_name_only(self) -> None:
        # "dawn" must not match "dawnsdk"; "dawn*" does.
        path = self.log("dawnsdk")
        self.assertEqual(run_checker(["--log", str(path), "--forbid", "dawn"])[0], 0)
        self.assertEqual(run_checker(["--log", str(path), "--forbid", "dawn*"])[0], 1)

    def test_a_missing_required_request_fails(self) -> None:
        path = self.log("Threads")
        code, _, err = run_checker(["--log", str(path), "--require", "skia"])
        self.assertEqual(code, 1)
        self.assertIn("required 'skia' was never requested", err)

    def test_an_empty_log_is_an_error_not_a_pass(self) -> None:
        path = write_log(self.tmp, "")
        code, _, err = run_checker(["--log", str(path), "--forbid", "skia"])
        self.assertEqual(code, 2)
        self.assertIn("recorder did not run", err)

    def test_a_missing_or_malformed_log_is_an_error(self) -> None:
        code, _, err = run_checker(["--log", str(self.tmp / "absent.jsonl"), "--forbid", "x"])
        self.assertEqual(code, 2)
        self.assertIn("cannot read", err)
        path = write_log(self.tmp, "{not json}\n")
        self.assertEqual(run_checker(["--log", str(path), "--forbid", "x"])[0], 2)
        path = write_log(self.tmp, json.dumps({"method": "FIND_PACKAGE"}) + "\n")
        self.assertEqual(run_checker(["--log", str(path), "--forbid", "x"])[0], 2)

    def test_at_least_one_rule_is_required(self) -> None:
        path = self.log("Threads")
        with self.assertRaises(SystemExit) as cm, contextlib.redirect_stderr(io.StringIO()):
            drc.main(["--log", str(path)])
        self.assertEqual(cm.exception.code, 2)


def cmake_version() -> tuple[int, int] | None:
    exe = shutil.which("cmake")
    if not exe:
        return None
    try:
        out = subprocess.run(
            [exe, "--version"], capture_output=True, text=True, encoding="utf-8", check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    first = out.split()[2] if len(out.split()) > 2 else ""
    parts = first.split(".")
    try:
        return int(parts[0]), int(parts[1])
    except (IndexError, ValueError):
        return None


FIXTURE = textwrap.dedent(
    """\
    cmake_minimum_required(VERSION 3.24)
    project(recorder_fixture LANGUAGES NONE)
    find_package(PulpRecorderAbsentPackage QUIET)
    include(FetchContent)
    FetchContent_Declare(viaprovider SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/viaprovider)
    FetchContent_MakeAvailable(viaprovider)
    FetchContent_Declare(viasourcedir SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/missing)
    FetchContent_MakeAvailable(viasourcedir)
    FetchContent_Declare(neverrequested SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/missing)
    if(COMMAND dependency_request_recorder_note)
        dependency_request_recorder_note(PROJECT_PROVISION Widget)
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/outcome.txt"
        "absent=${PulpRecorderAbsentPackage_FOUND};viaprovider=${VIAPROVIDER_ADDED};viasourcedir=${VIASOURCEDIR_ADDED}")
    """
)


@unittest.skipIf(
    (cmake_version() or (0, 0)) < (3, 24),
    "CMake 3.24 or newer is not on PATH; the recorder cases did not run",
)
class RecorderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-dep-recorder-"))
        self.src = self.tmp / "src"
        self.src.mkdir()
        (self.src / "CMakeLists.txt").write_text(FIXTURE, encoding="utf-8")
        for name in ("viaprovider", "override"):
            (self.src / name).mkdir()
            flag = "VIAPROVIDER_ADDED" if name == "viaprovider" else "VIASOURCEDIR_ADDED"
            (self.src / name / "CMakeLists.txt").write_text(
                f'set({flag} TRUE PARENT_SCOPE)\n', encoding="utf-8"
            )

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def configure(self, build: Path, *, recorder: bool = True, log: Path | None = None) -> Path:
        args = [
            "cmake", "-S", str(self.src), "-B", str(build),
            f"-DFETCHCONTENT_SOURCE_DIR_VIASOURCEDIR={self.src / 'override'}",
            f"-DFETCHCONTENT_SOURCE_DIR_NEVERREQUESTED={self.src / 'override'}",
        ]
        if recorder:
            args.append(f"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={RECORDER}")
        if log is not None:
            args.append(f"-DDEPENDENCY_REQUEST_LOG={log}")
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return build

    @staticmethod
    def entries(path: Path) -> list[tuple[str, str]]:
        return [
            (e["method"], e["name"])
            for e in map(json.loads, path.read_text(encoding="utf-8").splitlines())
        ]

    def test_every_kind_of_request_is_logged(self) -> None:
        build = self.configure(self.tmp / "build")
        entries = self.entries(build / "dependency-requests.jsonl")
        self.assertIn(("FIND_PACKAGE", "PulpRecorderAbsentPackage"), entries)
        self.assertIn(("FETCHCONTENT_MAKEAVAILABLE_SERIAL", "viaprovider"), entries)
        # CMake skips the provider for a source-dir override; the end-of-
        # configure pass still records it, and only once it was populated.
        self.assertIn(("FETCHCONTENT_SOURCE_DIR", "viasourcedir"), entries)
        self.assertNotIn(("FETCHCONTENT_SOURCE_DIR", "neverrequested"), entries)
        self.assertIn(("PROJECT_PROVISION", "Widget"), entries)
        self.assertEqual(len(entries), 4, entries)

    def test_the_recorder_changes_nothing_about_the_configure(self) -> None:
        with_recorder = self.configure(self.tmp / "with")
        without = self.configure(self.tmp / "without", recorder=False)
        expected = "absent=0;viaprovider=TRUE;viasourcedir=TRUE"
        self.assertEqual((with_recorder / "outcome.txt").read_text(encoding="utf-8"), expected)
        self.assertEqual((without / "outcome.txt").read_text(encoding="utf-8"), expected)
        # Control for the case above: with no recorder there is no log at all.
        self.assertFalse((without / "dependency-requests.jsonl").exists())

    def test_a_reconfigure_starts_a_fresh_log(self) -> None:
        build = self.configure(self.tmp / "build")
        first = self.entries(build / "dependency-requests.jsonl")
        self.configure(build)
        self.assertEqual(self.entries(build / "dependency-requests.jsonl"), first)

    def test_the_log_path_can_be_chosen(self) -> None:
        log = self.tmp / "elsewhere" / "requests.jsonl"
        log.parent.mkdir()
        self.configure(self.tmp / "build", log=log)
        self.assertTrue(log.exists())
        code, out, _ = run_checker(
            ["--log", str(log), "--require", "viaprovider", "--forbid", "skia"]
        )
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    unittest.main()
