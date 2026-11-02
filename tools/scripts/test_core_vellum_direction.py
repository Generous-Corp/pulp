#!/usr/bin/env python3
"""Positive and negative controls for core_vellum_direction.py.

Every planted violation must turn the check red, the real repository must stay
green, and a tree with nothing to scan must be reported as a scan error rather
than a clean tree.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SPEC = importlib.util.spec_from_file_location("core_vellum_direction",
                                              HERE / "core_vellum_direction.py")
direction = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = direction
SPEC.loader.exec_module(direction)
include_boundary = direction.include_boundary

REPO = HERE.parents[1]


class CoreVellumDirectionTests(unittest.TestCase):
    def setUp(self):
        # Synthetic trees hold none of the repository's allowlisted files, so
        # they are scanned with empty allowlists; tearDown restores the real ones.
        self.repo_include_allowed = dict(include_boundary.ALLOWED["core-vellum"])
        self.repo_cmake_allowed = dict(direction.ALLOWED_CMAKE)
        include_boundary.ALLOWED["core-vellum"].clear()
        direction.ALLOWED_CMAKE.clear()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self._write("core/audio/CMakeLists.txt",
                    "# Vellum consumes this module; prose may say Vellum::Gpu.\n"
                    "add_library(pulp-audio STATIC src/buffer.cpp)\n"
                    "target_link_libraries(pulp-audio PUBLIC pulp::runtime)\n")
        self._write("core/audio/src/buffer.cpp",
                    "// vellum/graphics/x.hpp is mentioned in prose only\n"
                    "#include <pulp/audio/buffer.hpp>\n")

    def tearDown(self):
        include_boundary.ALLOWED["core-vellum"].clear()
        include_boundary.ALLOWED["core-vellum"].update(self.repo_include_allowed)
        direction.ALLOWED_CMAKE.clear()
        direction.ALLOWED_CMAKE.update(self.repo_cmake_allowed)
        self.tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def _run(self, root: pathlib.Path | None = None) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return direction.main(["--root", str(root or self.root)])

    def test_clean_synthetic_tree_passes(self):
        self.assertEqual(self._run(), 0)

    def test_planted_includes_fail(self):
        for spelling in ("#include <vellum/graphics/dawn_bootstrap.hpp>\n",
                         '#include "vellum/view/view.hpp"\n',
                         "#import <vellum/app_host/host.hpp>\n",
                         '#include "../../external/vellum/foundation/x.hpp"\n'):
            with self.subTest(spelling=spelling):
                self._write("core/audio/src/planted.cpp", spelling)
                self.assertEqual(self._run(), 1)
        (self.root / "core/audio/src/planted.cpp").unlink()
        self.assertEqual(self._run(), 0)

    def test_planted_cmake_references_fail(self):
        for line in ("find_package(Vellum 0.2 CONFIG REQUIRED)\n",
                     "target_link_libraries(pulp-audio PRIVATE Vellum::Gpu)\n",
                     "target_link_libraries(pulp-audio PRIVATE vellum::view)\n",
                     "FetchContent_Declare(vellum GIT_REPOSITORY x)\n",
                     "add_subdirectory(${CMAKE_SOURCE_DIR}/external/vellum vellum)\n",
                     "find_package(\n  Vellum 0.2 CONFIG REQUIRED\n)\n",
                     "FetchContent_Declare(\n  vellum\n  GIT_REPOSITORY x\n)\n",
                     "add_subdirectory(\n  ${CMAKE_SOURCE_DIR}/external/vellum\n  vellum\n)\n",
                     "target_link_libraries(\n  pulp-audio PRIVATE\n  Vellum::Gpu\n)\n"):
            with self.subTest(line=line):
                self._write("core/audio/cmake/planted.cmake", line)
                self.assertEqual(self._run(), 1)

    def test_multiline_comments_are_not_references(self):
        self._write("core/audio/cmake/prose.cmake",
                    "find_package(\n"
                    "  # Vellum is prose here\n"
                    "  OtherPackage\n"
                    ")\n"
                    "#[[ FetchContent_Declare(\n"
                    "  vellum\n"
                    ") ]]\n")
        self.assertEqual(self._run(), 0)

    def test_comments_are_not_references(self):
        self._write("core/audio/cmake/prose.cmake",
                    "# find_package(Vellum) is forbidden here\n"
                    "#[[ target_link_libraries(x Vellum::Gpu) ]]\n"
                    'message(STATUS "a # inside a string") # Vellum::Gpu\n')
        self.assertEqual(self._run(), 0)

    def test_cmake_allowlist_counts_are_exact(self):
        self._write("core/gpu/CMakeLists.txt",
                    "target_link_libraries(pulp-gpu PRIVATE Vellum::Gpu)\n")
        direction.ALLOWED_CMAKE[("core/gpu/CMakeLists.txt", "Vellum::Gpu")] = (1, "known edge")
        self.assertEqual(self._run(), 0)
        # A second reference hidden behind the allowlisted one fails.
        self._write("core/gpu/CMakeLists.txt",
                    "target_link_libraries(pulp-gpu PRIVATE Vellum::Gpu)\n"
                    "target_link_libraries(pulp-audio PUBLIC Vellum::Gpu)\n")
        self.assertEqual(self._run(), 1)
        # Removing the edge without removing the entry fails as stale.
        self._write("core/gpu/CMakeLists.txt", "add_library(pulp-gpu STATIC x.cpp)\n")
        self.assertEqual(self._run(), 1)

    def test_stale_include_allowlist_fails(self):
        include_boundary.ALLOWED["core-vellum"][
            ("core/audio/src/buffer.cpp", "vellum/graphics/dawn_bootstrap.hpp")] = "known edge"
        self.assertEqual(self._run(), 1)

    def test_empty_tree_is_a_scan_error(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(self._run(pathlib.Path(empty)), 2)

    def test_repository_passes_with_its_allowlist(self):
        include_boundary.ALLOWED["core-vellum"].update(self.repo_include_allowed)
        direction.ALLOWED_CMAKE.update(self.repo_cmake_allowed)
        self.assertEqual(self._run(REPO), 0)

    def test_repository_fails_without_its_allowlist(self):
        # Control: the known edge is real, so the scan of the real tree can see it.
        self.assertEqual(self._run(REPO), 1)


if __name__ == "__main__":
    unittest.main()
