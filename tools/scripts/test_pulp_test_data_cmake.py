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
- a stale `<exe>.inputs.json` from an earlier configure is removed;
- NO_DEFINE records paths without touching the target's definitions, NONE
  records a reviewed source with no paths, and SOURCES narrows a declaration
  to some of an executable's sources (and refuses one that is not its own);
- `pulp_test_spawns()` gives a test an edge to a tool defined after the test
  directory, where an inline `if(TARGET tool)` is false and silently drops
  it; `runtime_targets` lists the edge, and `spawns_none` a reviewed NONE;
- a `$<TARGET_FILE:x>` definition without an edge to x fails the configure,
  and so does a NONE on an executable that has one, or a pulp_test_spawns()
  edge that does not reach the written index.

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

    def test_no_define_none_and_sources(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            extra = ("add_executable(multi m1.c m2.c)\n"
                     "pulp_test_data(multi NO_DEFINE SOURCES m1.c PATHS fixtures/other.json)\n"
                     "add_executable(reviewed r.c)\n"
                     "pulp_test_data(reviewed NONE)\n")
            src = self.project(Path(t), extra=extra)
            for name in ("m1", "m2", "r"):
                write(src, f"test/{name}.c", "int main(void) { return 0; }\n")
            build = Path(t) / "build"
            proc = self.configure(src, build)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            out = build / "test" / "test-data"
            multi = json.loads((out / "multi.inputs.json").read_text(encoding="utf-8"))
            self.assertEqual((multi["sources"], multi["inputs"]), (["test/m1.c"], ["fixtures/other.json"]))
            reviewed = json.loads((out / "reviewed.inputs.json").read_text(encoding="utf-8"))
            self.assertEqual((reviewed["sources"], reviewed["inputs"]), (["test/r.c"], []))
            for target in ("multi", "reviewed"):
                flags = (build / "test" / "CMakeFiles" / f"{target}.dir" / "flags.make").read_text(encoding="utf-8")
                self.assertNotIn("PULP_SOURCE_DIR", flags, target)

    def test_sources_must_belong_to_the_executable(self) -> None:
        with tempfile.TemporaryDirectory() as t:
            src = self.project(Path(t), extra="pulp_test_data(quiet SOURCES solo.c PATHS fixtures)")
            proc = self.configure(src, Path(t) / "build")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("'solo.c' is not one of its sources", proc.stderr)
            src2 = self.project(Path(t) / "b", extra="pulp_test_data(quiet NONE PATHS fixtures)")
            self.assertIn("NONE and PATHS are exclusive", self.configure(src2, Path(t) / "build2").stderr)

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


SPAWN_ROOT_LISTS = """cmake_minimum_required(VERSION 3.24)
project(pulp_test_spawns_fixture C)
add_subdirectory(test)
add_subdirectory(tools)
"""


@unittest.skipUnless(shutil.which("cmake") and shutil.which("make"), "cmake or make not on PATH")
class PulpTestSpawnsCMakeTests(unittest.TestCase):
    """Tools are defined after the test directory, as in the real tree."""

    def configure(self, extra: str) -> tuple[subprocess.CompletedProcess[str], dict]:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        tmp = Path(holder.name)
        src = tmp / "src"
        write(src, "CMakeLists.txt", SPAWN_ROOT_LISTS)
        write(src, "test/CMakeLists.txt",
              f'include("{MODULE.as_posix()}")\npulp_test_data_arm()\n'
              "add_executable(spawner s.c)\nadd_executable(plain p.c)\n" + extra)
        write(src, "tools/CMakeLists.txt", "add_executable(tool t.c)\n")
        for rel in ("test/s.c", "test/p.c", "tools/t.c"):
            write(src, rel, "int main(void) { return 0; }\n")
        build = tmp / "build"
        proc = subprocess.run(["cmake", "-G", "Unix Makefiles", "-S", str(src), "-B", str(build)],
                              capture_output=True, text=True, timeout=180)
        proc.stderr = " ".join(proc.stderr.split())
        index_path = build / "test" / "test-data" / "executables.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))["executables"] if index_path.exists() else {}
        return proc, index

    def test_a_deferred_edge_reaches_a_tool_defined_later(self) -> None:
        proc, index = self.configure(
            "pulp_test_spawns(spawner tool)\n"
            # The shape that silently dropped edges: the tool does not exist yet.
            "if(TARGET tool)\n  add_dependencies(plain tool)\nendif()\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(index["spawner"]["runtime_targets"], ["tool"])
        self.assertEqual(index["plain"]["runtime_targets"], [])
        self.assertEqual((index["spawner"]["spawns_none"], index["plain"]["spawns_none"]), (False, False))

    def test_a_target_file_definition_needs_an_edge(self) -> None:
        defs = 'target_compile_definitions(spawner PRIVATE TOOL="$<TARGET_FILE:tool>")\n'
        proc, _ = self.configure(defs)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("spawner -> tool", proc.stderr)
        # The inline guard evaluated before the tool exists adds nothing, so it
        # fails the same way.
        proc, _ = self.configure(defs + "if(TARGET tool)\n  add_dependencies(spawner tool)\nendif()\n")
        self.assertIn("spawner -> tool", proc.stderr)
        proc, index = self.configure(defs + "pulp_test_spawns(spawner tool)\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(index["spawner"]["runtime_targets"], ["tool"])

    def test_none_records_a_review_and_refuses_an_edge(self) -> None:
        proc, index = self.configure("pulp_test_spawns(plain NONE)\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual((index["plain"]["spawns_none"], index["plain"]["runtime_targets"]), (True, []))
        proc, _ = self.configure("pulp_test_spawns(spawner NONE)\npulp_test_spawns(spawner tool)\n")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("spawner is declared pulp_test_spawns(NONE) but depends on tool", proc.stderr)

    def test_a_declared_edge_must_reach_the_index(self) -> None:
        # A utility target is not something a test runs, so its edge cannot be
        # recorded as a runtime target; the configure says so instead of
        # dropping it.
        proc, _ = self.configure("add_custom_target(stage)\npulp_test_spawns(spawner stage)\n")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("spawner declares stage, which is not a recorded runtime target", proc.stderr)


if __name__ == "__main__":
    unittest.main()
