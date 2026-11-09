#!/usr/bin/env python3
"""Unit tests for confirm_failure_driver_link.py over captured tool output.

The end-to-end cases that need a real generator live in
test_confirm_failure.sh; these pin each parser to the text CMake and ninja
actually write, so a layout change fails here with the line that moved.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import confirm_failure_driver_link as link  # noqa: E402

NINJA_QUERY = """\
fixture_driver:
  input: CXX_EXECUTABLE_LINKER__fixture_driver_Release
    CMakeFiles/fixture_driver.dir/driver.cpp.o
    | libfixture_lib.a
    | /opt/homebrew/lib/libz.dylib
    || libfixture_lib.a
    |@ validation_stamp
  outputs:
    all
libfixture_lib.a:
  input: CXX_STATIC_LIBRARY_LINKER__fixture_lib_Release
    CMakeFiles/fixture_lib.dir/value.cpp.o
  outputs:
    fixture_driver
"""

NINJA_DEPS = """\
CMakeFiles/fixture_driver.dir/driver.cpp.o: #deps 3, deps mtime 1794254771355971660 (STALE)
    /SDK/SDKSettings.json
    /src/driver.cpp
    /src/shared.hpp

CMakeFiles/fixture_lib.dir/value.cpp.o: #deps 1, deps mtime 1794254771232216523 (VALID)
    /src/value.cpp

CMakeFiles/other.dir/other.cpp.o: deps not found
"""

MAKEFILE2 = """\
# CMAKE generated file: DO NOT EDIT!
all: CMakeFiles/fixture_lib.dir/all
all: CMakeFiles/fixture_driver.dir/all
.PHONY : all

CMakeFiles/fixture_lib.dir/all:
\t$(MAKE) $(MAKESILENT) -f CMakeFiles/fixture_lib.dir/build.make CMakeFiles/fixture_lib.dir/depend
CMakeFiles/fixture_subject.dir/all: CMakeFiles/fixture_lib.dir/all
CMakeFiles/fixture_driver.dir/all: CMakeFiles/fixture_subject.dir/all
CMakeFiles/d.dir/all: CMakeFiles/fixture_lib.dir/all

CMakeFiles/fixture_driver.dir/rule: cmake_check_build_system
\t$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 CMakeFiles/fixture_driver.dir/all
fixture_driver: CMakeFiles/fixture_driver.dir/rule
.PHONY : fixture_driver
fixture_subject: CMakeFiles/fixture_subject.dir/rule
d : CMakeFiles/d.dir/rule
.PHONY : d
"""

BUILD_MAKE = """\
# Object files for target fixture_lib
CMakeFiles/fixture_lib.dir/value.cpp.o: CMakeFiles/fixture_lib.dir/flags.make
CMakeFiles/fixture_lib.dir/value.cpp.o: /src/value.cpp
CMakeFiles/fixture_lib.dir/value.cpp.o: CMakeFiles/fixture_lib.dir/compiler_depend.ts
\t$(CMAKE_COMMAND) -E cmake_echo_color "Building CXX object"
"""


class NinjaParsers(unittest.TestCase):
    def test_query_keeps_explicit_and_implicit_inputs_only(self):
        inputs = link.parse_ninja_query(NINJA_QUERY)
        self.assertEqual(inputs["fixture_driver"], [
            "CMakeFiles/fixture_driver.dir/driver.cpp.o",
            "libfixture_lib.a",
            "/opt/homebrew/lib/libz.dylib",
        ])
        self.assertEqual(inputs["libfixture_lib.a"],
                         ["CMakeFiles/fixture_lib.dir/value.cpp.o"])

    def test_query_outputs_are_not_inputs(self):
        self.assertNotIn("all", link.parse_ninja_query(NINJA_QUERY)["fixture_driver"])

    def test_deps_records_and_missing_record(self):
        deps = link.parse_ninja_deps(NINJA_DEPS)
        self.assertEqual(deps["CMakeFiles/fixture_driver.dir/driver.cpp.o"],
                         ["/SDK/SDKSettings.json", "/src/driver.cpp", "/src/shared.hpp"])
        self.assertEqual(deps["CMakeFiles/fixture_lib.dir/value.cpp.o"], ["/src/value.cpp"])
        self.assertIsNone(deps["CMakeFiles/other.dir/other.cpp.o"])

    def test_inputs_split_by_line_keeps_spaces(self):
        self.assertEqual(link.parse_ninja_inputs("a.o\n/dir with space/b.cpp\n\n"),
                         ["a.o", "/dir with space/b.cpp"])


class LinkTxt(unittest.TestCase):
    def test_executable_output_and_inputs(self):
        outputs, inputs = link.parse_link_txt(
            "/usr/bin/c++ -O3 CMakeFiles/d.dir/driver.cpp.o -o fixture_driver  libfixture_lib.a\n")
        self.assertEqual(outputs, {"fixture_driver"})
        self.assertEqual(inputs, ["CMakeFiles/d.dir/driver.cpp.o", "libfixture_lib.a"])

    def test_archive_output_follows_ar_flags(self):
        outputs, inputs = link.parse_link_txt(
            "/usr/bin/ar qc libfixture_lib.a CMakeFiles/fixture_lib.dir/value.cpp.o\n"
            "/usr/bin/ranlib libfixture_lib.a\n")
        self.assertEqual(outputs, {"libfixture_lib.a"})
        self.assertEqual(inputs, ["CMakeFiles/fixture_lib.dir/value.cpp.o"])

    def test_response_file_is_expanded(self):
        rsp = {"CMakeFiles/d.dir/objects1.rsp": "a.cpp.o 'b c.cpp.o'"}
        outputs, inputs = link.parse_link_txt(
            "/usr/bin/c++ @CMakeFiles/d.dir/objects1.rsp -o d\n", rsp.__getitem__)
        self.assertEqual(outputs, {"d"})
        self.assertEqual(inputs, ["a.cpp.o", "b c.cpp.o"])

    def test_unreadable_response_file_is_skipped(self):
        def missing(_path):
            raise OSError("gone")
        self.assertEqual(link.parse_link_txt("c++ @x.rsp -o d\n", missing), ({"d"}, []))

    def test_wl_arguments_are_split_on_commas(self):
        _, inputs = link.parse_link_txt(
            "c++ x.o -Wl,-force_load,lib/libforced.a -Wl,-search_paths_first -o d\n")
        self.assertEqual(inputs, ["x.o", "lib/libforced.a"])

    def test_quoted_paths_and_bad_lines(self):
        outputs, inputs = link.parse_link_txt(
            'c++ "dir with space/x.o" -o "out dir/d" libz.so.1.2\n'
            'c++ "unterminated\n')
        self.assertEqual(outputs, {"out dir/d"})
        self.assertEqual(inputs, ["dir with space/x.o", "libz.so.1.2"])


class Makefile2(unittest.TestCase):
    def setUp(self):
        self.starts, self.edges = link.parse_makefile2(MAKEFILE2)

    def test_rule_entry_and_closure(self):
        self.assertEqual(self.starts["fixture_driver"], {"CMakeFiles/fixture_driver.dir"})
        self.assertEqual(link.makefile2_closure(self.starts, self.edges, "fixture_driver"), {
            "CMakeFiles/fixture_driver.dir", "CMakeFiles/fixture_subject.dir",
            "CMakeFiles/fixture_lib.dir"})

    def test_single_character_target_has_space_before_colon(self):
        self.assertEqual(self.starts["d"], {"CMakeFiles/d.dir"})
        self.assertEqual(link.makefile2_closure(self.starts, self.edges, "d"),
                         {"CMakeFiles/d.dir", "CMakeFiles/fixture_lib.dir"})

    def test_all_seeds_from_its_dir_all_lines(self):
        self.assertIn("CMakeFiles/fixture_driver.dir",
                      link.makefile2_closure(self.starts, self.edges, "all"))

    def test_subject_target_does_not_build_the_driver(self):
        self.assertNotIn("CMakeFiles/fixture_driver.dir",
                         link.makefile2_closure(self.starts, self.edges, "fixture_subject"))

    def test_unknown_target_has_an_empty_closure(self):
        self.assertEqual(link.makefile2_closure(self.starts, self.edges, "nope"), set())

    def test_build_make_rules(self):
        rules = link.parse_build_make(BUILD_MAKE)
        self.assertIn(("CMakeFiles/fixture_lib.dir/value.cpp.o", "/src/value.cpp"), rules)
        self.assertEqual(len(rules), 3)


class Drivers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = os.path.realpath(self.tmp.name)
        self.build = os.path.join(root, "build")
        os.makedirs(self.build)
        for name in ("driver", "subject", "data.wav"):
            open(os.path.join(self.build, name), "w").close()
        open(os.path.join(root, "run.sh"), "w").close()
        self.root = root

    def tearDown(self):
        self.tmp.cleanup()

    def pick(self, cmd):
        return link.pick_drivers(cmd, self.build, "build/subject", cwd=self.root)

    def test_wrapper_prefixes_do_not_hide_the_driver(self):
        driver = os.path.join(self.build, "driver")
        self.assertEqual(self.pick("env ./build/driver"), [driver])
        self.assertEqual(self.pick("VAR=1 ./build/driver '[tag]'"), [driver])

    def test_a_chain_after_the_subject_still_checks_the_driver(self):
        self.assertEqual(self.pick("./build/subject && ./build/driver"),
                         [os.path.join(self.build, "driver")])

    def test_the_subject_alone_needs_no_check(self):
        self.assertEqual(self.pick("./build/subject"), [])

    def test_no_build_dir_file_is_unknown(self):
        for cmd in ("sh ./run.sh", "ctest --test-dir build -R x", "python3 -c pass"):
            with self.assertRaises(link.Unknown, msg=cmd):
                self.pick(cmd)

    def test_a_build_dir_data_file_counts_as_a_driver(self):
        self.assertEqual(self.pick("./build/driver ./build/data.wav"),
                         [os.path.join(self.build, "driver"),
                          os.path.join(self.build, "data.wav")])


class EditIdentity(unittest.TestCase):
    def test_source_edit_matches_its_own_source_only(self):
        edit = link.Edit(file="/src/value.cpp", obj_name="value.cpp.o")
        self.assertTrue(edit.carried_by("lib.dir/value.cpp.o", ["/src/value.cpp"]))
        self.assertFalse(edit.carried_by("drv.dir/value.cpp.o", ["/src/driver_src/value.cpp"]))
        self.assertFalse(edit.carried_by("lib.dir/other.cpp.o", ["/src/value.cpp"]))

    def test_object_path_matches_that_source_exactly(self):
        edit = link.Edit(file="/src/value_data.txt", obj_name="value.cpp.o",
                         object_source="/src/value.cpp")
        self.assertTrue(edit.carried_by("lib.dir/value.cpp.o", ["/src/value.cpp"]))
        self.assertFalse(edit.carried_by("drv.dir/value.cpp.o", ["/src/driver_src/value.cpp"]))

    def test_bare_object_name_matches_any_object_of_that_name(self):
        edit = link.Edit(file="/src/prelude.js", obj_name="gen.cpp.o", by_object_name=True)
        self.assertTrue(edit.carried_by("a.dir/gen.cpp.o", ["/build/gen.cpp"]))
        self.assertFalse(edit.carried_by("a.dir/other.cpp.o", ["/build/gen.cpp"]))


class FakeNinja:
    """Replays captured `ninja -t` output for NinjaGraph."""

    def __init__(self, outputs):
        self.outputs = outputs

    def __call__(self, argv):
        tool = argv[argv.index("-t") + 1]
        text = self.outputs.get(tool)
        return subprocess.CompletedProcess(argv, 0 if text is not None else 1,
                                           text or "", "")


class NinjaGraphWalk(unittest.TestCase):
    build = "/b"

    def graph(self, outputs):
        return link.NinjaGraph(self.build, "ninja", run=FakeNinja(outputs))

    def test_walk_reaches_library_objects(self):
        g = self.graph({"query": NINJA_QUERY})
        self.assertEqual(g.objects("/b/fixture_driver"), [
            "CMakeFiles/fixture_driver.dir/driver.cpp.o",
            "CMakeFiles/fixture_lib.dir/value.cpp.o"])

    def test_a_node_with_no_objects_is_unknown(self):
        g = self.graph({"query": "run.sh:\n  input: CUSTOM_COMMAND\n    ../run.sh.in\n"})
        with self.assertRaises(link.Unknown):
            g.objects("/b/run.sh")

    def test_header_edit_found_in_deps(self):
        g = self.graph({"deps": NINJA_DEPS})
        edit = link.Edit(file="/src/shared.hpp", obj_name="shared.hpp.o", is_header=True)
        self.assertEqual(g.carrying_object(edit, ["CMakeFiles/fixture_driver.dir/driver.cpp.o"]),
                         "CMakeFiles/fixture_driver.dir/driver.cpp.o")

    def test_header_edit_with_a_missing_record_is_unknown(self):
        g = self.graph({"deps": NINJA_DEPS})
        edit = link.Edit(file="/src/shared.hpp", obj_name="shared.hpp.o", is_header=True)
        with self.assertRaises(link.Unknown):
            g.carrying_object(edit, ["CMakeFiles/other.dir/other.cpp.o"])


class Main(unittest.TestCase):
    def test_a_build_dir_with_no_graph_exits_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = os.path.join(tmp, "build")
            os.makedirs(build)
            open(os.path.join(build, "driver"), "w").close()
            status = link.main(["--build-dir", build, "--file", __file__,
                                "--obj-name", "x.cpp.o", "--test", f"{build}/driver",
                                "--target", "t", "--subject", f"{build}/subject"])
        self.assertEqual(status, 2)

    def test_a_crash_exits_2(self):
        original = link.decide
        link.decide = lambda *a, **k: (_ for _ in ()).throw(UnicodeDecodeError(
            "utf-8", b"\xff", 0, 1, "invalid"))
        try:
            status = link.main(["--build-dir", "/", "--file", __file__,
                                "--obj-name", "x.o", "--test", "x", "--target", "t",
                                "--subject", "s"])
        finally:
            link.decide = original
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
