#!/usr/bin/env python3
"""The commit-bound declaration is applied by CMake itself, not by authors.

`_pulp_declare_commit_bound` (tools/cmake/PulpControlShipping.cmake) is called
by every helper that embeds a per-configure build identity. This configures
and builds a two-executable project that uses Pulp's real catch_discover_tests
and the real helper, then reads ctest's own listing:

- every test discovered from a declared target carries `commit-bound` beside
  the labels its registration passed; a test of an undeclared target does not;
- the declaration is written to <build>/pulp-commit-bound/<target>.json naming
  the target's file, which the CI reuse record reads; an undeclared target has
  no such file;
- declaring a target twice writes it once.

The executables answer `--list-tests` the way a Catch2 binary does, so the
discovery script runs unchanged. Needs cmake and a C compiler.

Run:
    python3 tools/ci/test_commit_bound_cmake.py
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CMAKE_DIR = ROOT / "tools/cmake"

STUB = r"""
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--list-tests") == 0) { puts("first case"); puts("second case"); return 0; }
    return 0;
}
"""

PROJECT = """cmake_minimum_required(VERSION 3.24)
project(commit_bound C)
enable_testing()
include("{cmake}/PulpCatch.cmake")
include("{cmake}/PulpControlShipping.cmake")
add_executable(bound stub.c)
add_executable(plain stub.c)
_pulp_declare_commit_bound(bound)
_pulp_declare_commit_bound(bound)
catch_discover_tests(bound PROPERTIES LABELS "mine\\;gpu")
catch_discover_tests(plain PROPERTIES LABELS "mine" TEST_PREFIX "plain-")
"""


def labels(test: dict) -> set[str]:
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS":
            return set(prop.get("value") or [])
    return set()


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest") and (shutil.which("cc") or shutil.which("clang")),
                     "needs cmake, ctest and a C compiler")
class CommitBoundCMakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        src, build = Path(cls.tmp.name) / "src", Path(cls.tmp.name) / "build"
        src.mkdir()
        (src / "stub.c").write_text(STUB)
        (src / "CMakeLists.txt").write_text(PROJECT.format(cmake=CMAKE_DIR.as_posix()))
        env = {k: v for k, v in os.environ.items() if not k.startswith(("CMAKE_", "CTEST_"))}
        for cmd in (["cmake", "-S", str(src), "-B", str(build)], ["cmake", "--build", str(build), "-j2"]):
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=env)
            if proc.returncode:
                raise AssertionError(f"{cmd} failed:\n{proc.stdout}\n{proc.stderr}")
        listing = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                 capture_output=True, text=True, timeout=120, env=env, check=True)
        cls.tests = {t["name"]: t for t in json.loads(listing.stdout)["tests"]}
        cls.build = build

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_tests_of_a_declared_target_carry_the_label_beside_their_own(self) -> None:
        bound = {n: t for n, t in self.tests.items() if not n.startswith("plain-")}
        plain = {n: t for n, t in self.tests.items() if n.startswith("plain-")}
        # Control: discovery ran for both targets.
        self.assertEqual((len(bound), len(plain)), (2, 2), sorted(self.tests))
        for name, test in bound.items():
            self.assertEqual(labels(test), {"mine", "gpu", "commit-bound"}, name)
        for name, test in plain.items():
            self.assertEqual(labels(test), {"mine"}, name)

    def test_the_declaration_is_written_once_for_the_reuse_record(self) -> None:
        folder = self.build / "pulp-commit-bound"
        files = sorted(p.name for p in folder.iterdir())
        self.assertEqual(files, ["bound.json"])
        doc = json.loads((folder / "bound.json").read_text())
        self.assertEqual(doc["target"], "bound")
        self.assertEqual(os.path.realpath(doc["file"]), os.path.realpath(self.build / "bound"))


if __name__ == "__main__":
    unittest.main()
