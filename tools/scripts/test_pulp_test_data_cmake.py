#!/usr/bin/env python3
"""Tests for tools/cmake/PulpTestData.cmake (`pulp_test_data()`).

Configures a throwaway project whose `test/` directory declares three
executables the way the real manifests do, and reads what configure wrote.
What must hold:
- a standalone executable's declaration writes `<exe>.inputs.json` with its
  sources and paths, and defines PULP_SOURCE_DIR target-wide;
- a grouped member's declaration covers only that member's sources, and the
  definition lands on those sources, not on its neighbours in the group;
- `executables.json` lists every test executable's sources and the
  definitions that point into the checkout, but not those pointing into the
  build tree even when it lives inside the checkout;
- a path that matches nothing fails the configure; a glob that matches passes;
- a stale `<exe>.inputs.json` from an earlier configure is removed.

Run:
    python3 tools/scripts/test_pulp_test_data_cmake.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MODULE = REPO / "tools" / "cmake" / "PulpTestData.cmake"

ROOT_LISTS = """cmake_minimum_required(VERSION 3.24)
project(pulp_test_data_fixture C)
add_subdirectory(test)
"""

TEST_LISTS = """include("{module}")
pulp_test_data_arm()
add_executable(solo solo.c)
pulp_test_data(solo PATHS fixtures/solo.json)
add_executable(group g1.c g2.c)
_pulp_test_data_register_suite(member-one group g1.c)
_pulp_test_data_register_suite(member-two group g2.c)
pulp_test_data(member-one PATHS fixtures/*.json)
add_executable(quiet q.c)
target_compile_definitions(quiet PRIVATE
    CORPUS_DIR="${{CMAKE_SOURCE_DIR}}/fixtures" OUT_DIR="${{CMAKE_BINARY_DIR}}/out")
{extra}
"""


def write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


@unittest.skipUnless(shutil.which("cmake") and shutil.which("make"), "cmake or make not on PATH")
class PulpTestDataCMakeTests(unittest.TestCase):
    def project(self, tmp: Path, extra: str = "") -> Path:
        src = tmp / "src"
        write(src, "CMakeLists.txt", ROOT_LISTS)
        write(src, "test/CMakeLists.txt", TEST_LISTS.format(module=MODULE.as_posix(), extra=extra))
        for name in ("solo", "g1", "g2", "q"):
            write(src, f"test/{name}.c", "int main(void) { return 0; }\n")
        write(src, "fixtures/solo.json", "{}\n")
        write(src, "fixtures/other.json", "{}\n")
        return src

    def configure(self, src: Path, build: Path) -> subprocess.CompletedProcess[str]:
        proc = subprocess.run(["cmake", "-G", "Unix Makefiles", "-S", str(src), "-B", str(build)],
                              capture_output=True, text=True, timeout=180)
        # CMake wraps messages at 80 columns; compare on single spaces.
        proc.stderr = " ".join(proc.stderr.split())
        return proc

    def test_manifests_definitions_and_index(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            src = self.project(tmp)
            build = src / "build"  # inside the checkout, as a local build often is
            proc = self.configure(src, build)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            out = build / "test" / "test-data"
            solo = json.loads((out / "solo.inputs.json").read_text(encoding="utf-8"))
            self.assertEqual(solo, {"schema": "pulp-test-data-inputs/v1", "executable": "solo", "kind": "compiled",
                                    "sources": ["test/solo.c"], "inputs": ["fixtures/solo.json"]})
            group = json.loads((out / "group.inputs.json").read_text(encoding="utf-8"))
            self.assertEqual((group["sources"], group["inputs"]), (["test/g1.c"], ["fixtures/*.json"]))
            self.assertFalse((out / "quiet.inputs.json").exists())

            index = json.loads((out / "executables.json").read_text(encoding="utf-8"))["executables"]
            self.assertEqual(index["group"]["sources"], ["test/g1.c", "test/g2.c"])
            self.assertEqual(index["solo"]["tree_defines"], ["PULP_SOURCE_DIR"])
            self.assertEqual(index["group"]["tree_defines"], ["PULP_SOURCE_DIR"])
            self.assertEqual(index["quiet"]["tree_defines"], ["CORPUS_DIR"])  # OUT_DIR is the build tree

            flags = lambda rel: (build / "test" / "CMakeFiles" / rel / "flags.make").read_text(encoding="utf-8")
            self.assertIn("PULP_SOURCE_DIR=", flags("solo.dir"))
            group_flags = flags("group.dir")
            g1 = group_flags.split("# Custom defines: test/CMakeFiles/group.dir/g1.c.o_DEFINES")
            self.assertEqual(len(g1), 2, group_flags)
            self.assertIn("PULP_SOURCE_DIR=", g1[1].splitlines()[0])
            self.assertNotIn("g2.c.o_DEFINES", group_flags)  # the neighbour gets no definition

    def test_a_path_that_matches_nothing_fails_configure(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            src = self.project(Path(t), extra="pulp_test_data(quiet PATHS fixtures/missing.json)")
            proc = self.configure(src, Path(t) / "build")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("'fixtures/missing.json' matches nothing in the checkout", proc.stderr)
            src2 = self.project(Path(t) / "b", extra="pulp_test_data(quiet PATHS ../outside)")
            proc = self.configure(src2, Path(t) / "build2")
            self.assertIn("must be relative to the checkout root", proc.stderr)

    def test_a_dropped_declaration_removes_its_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            src = self.project(Path(t), extra="pulp_test_data(quiet PATHS fixtures)")
            build = Path(t) / "build"
            self.assertEqual(self.configure(src, build).returncode, 0)
            manifest = build / "test" / "test-data" / "quiet.inputs.json"
            self.assertTrue(manifest.exists())
            write(src, "test/CMakeLists.txt", TEST_LISTS.format(module=MODULE.as_posix(), extra=""))
            self.assertEqual(self.configure(src, build).returncode, 0)
            self.assertFalse(manifest.exists())


if __name__ == "__main__":
    unittest.main()
