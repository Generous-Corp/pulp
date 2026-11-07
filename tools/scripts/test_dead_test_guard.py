#!/usr/bin/env python3
"""Tests for tools/scripts/dead_test_guard.py, using throwaway git repositories."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dead_test_guard as guard  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]


def make_repo(files: dict[str, str]) -> tempfile.TemporaryDirectory:
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    return tmp


def dead(files: dict[str, str]) -> list[str]:
    with make_repo(files) as tmp:
        return guard.find_dead(Path(tmp))[0]


class DeadTestGuardTest(unittest.TestCase):
    def test_unreferenced_test_is_reported(self):
        self.assertEqual(
            dead({"test/CMakeLists.txt": "add_executable(a test_a.cpp)\n",
                  "test/test_a.cpp": "", "test/test_orphan.cpp": ""}),
            ["test/test_orphan.cpp"])

    def test_comment_does_not_count_as_a_reference(self):
        self.assertEqual(
            dead({"test/CMakeLists.txt": "# add_executable(a test_a.cpp)\n",
                  "test/test_a.cpp": ""}),
            ["test/test_a.cpp"])

    def test_cmake_file_in_subdirectory_resolves_against_parent(self):
        self.assertEqual(
            dead({"test/CMakeLists.txt": "include(cmake/more.cmake)\n",
                  "test/cmake/more.cmake": "add_executable(b test_b.cpp)\n",
                  "test/test_b.cpp": ""}),
            [])

    def test_variable_prefixed_path_in_a_subdirectory(self):
        files = {
            "test/CMakeLists.txt": "include(cmake/slice.cmake)\n",
            "test/cmake/slice.cmake":
                "set(_d ${CMAKE_CURRENT_SOURCE_DIR}/slice_dir)\n"
                "add_library(s STATIC ${_d}/widget.cpp)\n",
            "test/slice_dir/widget.cpp": "",
            "test/other_dir/widget.cpp": "",
        }
        self.assertEqual(dead(files), ["test/other_dir/widget.cpp"])

    def test_suite_name_derives_the_source_file(self):
        self.assertEqual(
            dead({"test/CMakeLists.txt": "pulp_add_test_suite(pulp-test-foo-bar LIBRARIES x)\n",
                  "test/test_foo_bar.cpp": ""}),
            [])

    def test_suite_with_sources_does_not_derive_a_name(self):
        self.assertEqual(
            dead({"test/CMakeLists.txt":
                  "pulp_add_test_suite(pulp-test-foo SOURCES other.cpp)\n",
                  "test/other.cpp": "", "test/test_foo.cpp": ""}),
            ["test/test_foo.cpp"])

    def test_plugin_entry_convention(self):
        files = {
            "test/CMakeLists.txt": "add_subdirectory(fixtures/p)\n",
            "test/fixtures/p/CMakeLists.txt": "pulp_add_plugin(P)\n",
            "test/fixtures/p/clap_entry.cpp": "",
            "test/fixtures/p/main.cpp": "",
            "test/fixtures/q/CMakeLists.txt": "add_library(q q_lib.cpp)\n",
            "test/fixtures/q/q_lib.cpp": "",
            "test/fixtures/q/clap_entry.cpp": "",
        }
        self.assertEqual(dead(files), ["test/fixtures/q/clap_entry.cpp"])

    def test_glob_is_a_reference_to_what_it_matches(self):
        files = {"test/CMakeLists.txt": "file(GLOB S fixtures/*.cpp)\n",
                 "test/fixtures/a.cpp": "", "test/fixtures/deep/b.cpp": ""}
        self.assertEqual(dead(files), ["test/fixtures/deep/b.cpp"])

    def test_templated_name_needs_a_distinctive_prefix(self):
        files = {
            "test/CMakeLists.txt":
                "add_library(l STATIC ui_package_${pkg}.cpp)\n"
                "add_executable(e test_${name}.cpp)\n",
            "test/ui_package_a.cpp": "", "test/test_zz.cpp": "",
        }
        self.assertEqual(dead(files), ["test/test_zz.cpp"])

    def test_sources_outside_the_test_tree_are_out_of_scope(self):
        self.assertEqual(dead({"CMakeLists.txt": "", "examples/x/test/t.cpp": "",
                               "src/test_s.cpp": ""}), [])

    def test_core_module_test_directories_are_in_scope(self):
        self.assertEqual(dead({"CMakeLists.txt": "", "core/audio/test/t.cpp": ""}),
                         ["core/audio/test/t.cpp"])

    def test_allow_list_entries_must_stay_current(self):
        files = {"test/CMakeLists.txt": "add_executable(a test_a.cpp)\n",
                 "test/test_a.cpp": ""}
        with make_repo(files) as tmp:
            allow = Path(tmp) / "allow.json"
            allow.write_text(json.dumps({"allow": [
                {"path": "test/test_a.cpp", "reason": "x"},
                {"path": "test/gone.cpp", "reason": "x"}]}))
            original = guard.ALLOW_FILE
            guard.ALLOW_FILE = allow
            try:
                _dead, stale = guard.find_dead(Path(tmp))
            finally:
                guard.ALLOW_FILE = original
        self.assertEqual(len(stale), 2)

    def test_repository_has_no_dead_tests(self):
        self.assertEqual(guard.find_dead(REPO_ROOT), ([], []))


if __name__ == "__main__":
    unittest.main()
