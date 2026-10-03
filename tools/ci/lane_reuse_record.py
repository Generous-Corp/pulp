#!/usr/bin/env python3
"""Write the local mac lane's reuse record for the run Shipyard is executing.

Live test reuse keys each executable against an earlier build's record, and
the record must come from a run on the same toolchain, so the lane keeps its
own (Shipyard's host-local store; the lane has no GitHub credentials). When a
target sets `reuse_record = true`, Shipyard exports a fresh directory as
SHIPYARD_REUSE_RECORD_DIR and files it afterwards only if a parsing job.json
is there. This writes that job.json and the rest of the record with the same
reuse_record.py flags build.yml uses.

Two callers:
- the lane's ordinary test stage: `lane_reuse_record.py test --build-dir B
  -- <ctest argv>` runs ctest, adding `--output-junit` into the record
  directory, then records the run as the `full` suite;
- run_changed_surface_tests.py, after a bounded plan's legs: `record()`.

Recording never changes a verdict: the test subcommand exits with ctest's
status, and a recorder failure is a warning. Without the variable nothing is
recorded and ctest runs exactly as given.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional, Sequence, Tuple

RECORD_DIR_ENV = "SHIPYARD_REUSE_RECORD_DIR"
HERE = Path(__file__).resolve().parent
RECORDER = HERE / "reuse_record.py"
# One suite: (name, its JUnit report, the LastTest.log copy or None, whether
# it ran with --repeat).
Suite = Tuple[str, Path, Optional[Path], bool]


def record_dir() -> Path | None:
    value = os.environ.get(RECORD_DIR_ENV, "").strip()
    return Path(value) if value else None


def suite_dir(out: Path, name: str) -> Path:
    path = out / "suites" / name
    path.mkdir(parents=True, exist_ok=True)
    return path


def keep_last_test_log(build_dir: Path, out: Path, name: str) -> Path | None:
    """Copy ctest's LastTest.log for suite `name` into the record; any later
    ctest call in the build directory rewrites it."""
    log = build_dir / "Testing" / "Temporary" / "LastTest.log"
    if not log.is_file():
        return None
    dest = suite_dir(out, name) / "LastTest.log"
    shutil.copyfile(log, dest)
    return dest


def recorder_argv(out: Path, build_dir: Path, source_root: Path, suites: Iterable[Suite],
                  build_outcome: str) -> list[str]:
    argv = [sys.executable, str(RECORDER), "write",
            "--run-kind", "lane", "--run-id", out.name,
            "--out-dir", str(out), "--build-dir", str(build_dir), "--source-root", str(source_root),
            "--build-outcome", build_outcome,
            "--link-members", "--object-deps", "--codemodel", "--inventory"]
    for name, junit, attempts, repeat in suites:
        spec = f"{name}={junit}"
        if attempts is not None:
            spec += f",attempts={attempts}"
        if repeat:
            spec += ",repeat"
        argv += ["--suite", spec]
    return argv


def record(build_dir: Path, source_root: Path, suites: Sequence[Suite], build_outcome: str = "success") -> int:
    """Write the record when Shipyard asked for one; 0 when it did not."""
    out = record_dir()
    if out is None:
        return 0
    rc = subprocess.run(recorder_argv(out, build_dir, source_root, suites, build_outcome)).returncode
    if rc != 0:
        print(f"lane-reuse-record: WARNING: reuse_record.py exited {rc}; this run leaves no record, "
              "so the next plan keyed against it runs in full", file=sys.stderr)
    return rc


def record_legs(build_dir: Path, source_root: Path,
                legs: Sequence[Tuple[str, Path, Optional[Path]]], build_outcome: str) -> int:
    """Record a bounded plan's legs (name, JUnit, LastTest.log copy). The
    runner keeps its JUnit reports in a private directory it deletes, so each
    is copied into the record first; a leg that wrote none is left out."""
    out = record_dir()
    if out is None:
        return 0
    suites: list[Suite] = []
    for name, junit, attempts in legs:
        if junit.is_file():
            dest = suite_dir(out, name) / "ctest.junit.xml"
            shutil.copyfile(junit, dest)
            suites.append((name, dest, attempts, True))
    return record(build_dir, source_root, suites, build_outcome)


def run_test_stage(build_dir: Path, ctest: list[str]) -> int:
    out = record_dir()
    if out is None:
        return subprocess.run(ctest).returncode
    junit = suite_dir(out, "full") / "ctest.junit.xml"
    rc = subprocess.run(ctest + ["--output-junit", str(junit)]).returncode
    attempts = keep_last_test_log(build_dir, out, "full")
    record(build_dir, Path.cwd(), [("full", junit, attempts, "--repeat" in ctest)])
    return rc


def main(argv: list[str]) -> int:
    if len(argv) < 5 or argv[1] != "test" or argv[2] != "--build-dir" or "--" not in argv:
        print("usage: lane_reuse_record.py test --build-dir DIR -- <ctest argv>", file=sys.stderr)
        return 2
    split = argv.index("--")
    ctest = argv[split + 1:]
    if not ctest:
        print("lane_reuse_record.py: no ctest command after --", file=sys.stderr)
        return 2
    return run_test_stage(Path(argv[3]), ctest)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
