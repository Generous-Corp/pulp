#!/usr/bin/env python3
"""Tests for tools/ci/object_deps.py.

    python3 tools/ci/test_object_deps.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import object_deps as od  # noqa: E402

SRC = Path("/w/src")
BUILD = Path("/w/src/build")

# `ninja -t deps` output: one valid object with an SDK file, its source, an
# in-tree header, a generated header and a pinned dependency's header; one
# STALE object; a relative dependency path (relative to the build dir).
DEPS = """core/CMakeFiles/lib.dir/a.cpp.o: #deps 5, deps mtime 1791050292689342147 (VALID)
    /SDK/usr/include/vector
    /w/src/core/a.cpp
    /w/src/core/include/a.hpp
    /w/src/build/generated/version.hpp
    /w/src/build/_deps/catch2-src/catch.hpp

core/CMakeFiles/lib.dir/b.cpp.o: #deps 2, deps mtime 1 (STALE)
    /w/src/core/b.cpp
    /w/src/core/include/a.hpp

test/CMakeFiles/t.dir/t.cpp.o: #deps 2, deps mtime 2 (VALID)
    ../test/t.cpp
    /w/src/core/include/a.hpp
"""

QUERY = """core/liblib.a:
  input: CXX_STATIC_LIBRARY_LINKER__lib_
    core/CMakeFiles/lib.dir/a.cpp.o
    core/CMakeFiles/lib.dir/sub/a.cpp.o
    core/CMakeFiles/lib.dir/b.cpp.o
    | core/implicit.stamp
    || core/order_only
  outputs:
    test/t
"""


class ParseTests(unittest.TestCase):
    def test_in_tree_dependencies_are_kept_and_the_rest_dropped(self) -> None:
        objects, stale = od.parse_deps(DEPS, BUILD, SRC)
        self.assertEqual(objects["<build>/core/CMakeFiles/lib.dir/a.cpp.o"],
                         ["<src>/core/a.cpp", "<src>/core/include/a.hpp", "<build>/generated/version.hpp"])
        # Relative to the build directory, like Ninja's own working directory.
        self.assertEqual(objects["<build>/test/CMakeFiles/t.dir/t.cpp.o"],
                         ["<src>/test/t.cpp", "<src>/core/include/a.hpp"])

    def test_a_stale_object_is_listed_and_given_no_dependencies(self) -> None:
        objects, stale = od.parse_deps(DEPS, BUILD, SRC)
        self.assertEqual(stale, ["<build>/core/CMakeFiles/lib.dir/b.cpp.o"])
        self.assertNotIn("<build>/core/CMakeFiles/lib.dir/b.cpp.o", objects)

    def test_archive_members_map_to_their_objects_and_share_a_name(self) -> None:
        got = od.parse_archive_inputs(QUERY, BUILD, SRC)
        self.assertEqual(got, {"<build>/core/liblib.a": {
            "a.cpp.o": ["<build>/core/CMakeFiles/lib.dir/a.cpp.o", "<build>/core/CMakeFiles/lib.dir/sub/a.cpp.o"],
            "b.cpp.o": ["<build>/core/CMakeFiles/lib.dir/b.cpp.o"]}})

    def test_compact_round_trips_through_expand(self) -> None:
        objects, stale = od.parse_deps(DEPS, BUILD, SRC)
        doc = od.compact(objects, stale, {})
        self.assertEqual({o: sorted(h) for o, h in od.expand(doc).items()},
                         {o: sorted(h) for o, h in objects.items()})
        self.assertEqual(len(doc["headers"]), 4)


class UnusableTests(unittest.TestCase):
    def test_reasons(self) -> None:
        good = od.compact({"<build>/a.o": ["<src>/a.h"]}, [], {})
        self.assertIsNone(od.unusable(good))
        self.assertEqual(od.unusable(None), "no record")
        self.assertEqual(od.unusable({**good, "schema": "pulp-object-deps/v9"}),
                         "unknown schema 'pulp-object-deps/v9'")
        self.assertEqual(od.unusable({**good, "objects": {}}), "no objects recorded")
        with self.assertRaises(ValueError):
            od.expand({**good, "schema": None})


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "needs cmake and ninja")
class RealBuildTests(unittest.TestCase):
    """A CMake + Ninja project: the header an object includes, and only it."""

    def test_a_real_build_records_who_includes_what(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src, build = Path(tmp) / "src", Path(tmp) / "build"
            (src / "a").mkdir(parents=True)
            (src / "b").mkdir()
            (src / "inc").mkdir()
            (src / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(t CXX)\n"
                "add_library(lib STATIC a/x.cpp b/x.cpp)\ntarget_include_directories(lib PUBLIC inc)\n"
                "add_executable(app main.cpp)\ntarget_link_libraries(app lib)\n")
            (src / "inc" / "h.hpp").write_text("int h();\n")
            (src / "a" / "x.cpp").write_text('#include "h.hpp"\nint h() { return 1; }\n')
            (src / "b" / "x.cpp").write_text("int b() { return 2; }\n")
            (src / "main.cpp").write_text('#include "h.hpp"\nint main() { return h(); }\n')
            for cmd in (["cmake", "-S", str(src), "-B", str(build), "-G", "Ninja"],
                        ["cmake", "--build", str(build), "-j", "2"]):
                subprocess.run(cmd, check=True, capture_output=True)
            doc = od.collect(build.resolve(), src.resolve())
        full = od.expand(doc)
        including = sorted(o for o, hs in full.items() if "<src>/inc/h.hpp" in hs)
        self.assertEqual(including, ["<build>/CMakeFiles/app.dir/main.cpp.o", "<build>/CMakeFiles/lib.dir/a/x.cpp.o"])
        self.assertEqual(sorted(doc["members"]["<build>/liblib.a"]["x.cpp.o"]),
                         ["<build>/CMakeFiles/lib.dir/a/x.cpp.o", "<build>/CMakeFiles/lib.dir/b/x.cpp.o"])

    def test_a_build_without_a_ninja_log_is_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(od.Unavailable):
                od.collect(Path(tmp), Path(tmp))


if __name__ == "__main__":
    unittest.main()
