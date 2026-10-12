#!/usr/bin/env python3
"""Positive and negative controls for include_boundary.py's view-core boundary.

Every include spelling the compiler accepts has its own planted crossing, so a
parser that recognises only one spelling turns this suite red. The foundation
boundary's controls live in test_foundation_include_lint.py.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("include_boundary.py")
SPEC = importlib.util.spec_from_file_location("include_boundary", MODULE_PATH)
boundary = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = boundary  # dataclasses resolve the module by name
SPEC.loader.exec_module(boundary)

REPO = pathlib.Path(__file__).resolve().parents[2]


class ViewCoreBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self._write("core/view/include/pulp/view/view.hpp", "#pragma once\n")
        self._write("core/view/src/view.cpp",
                    '#include <pulp/view/view.hpp>\n#include "pulp/canvas/canvas.hpp"\n'
                    '#include "../include/pulp/view/view.hpp"\n')
        self._write("core/view/audio/include/pulp/view/meter.hpp", "#pragma once\n")
        self._write("core/view/audio/src/meter.cpp",
                    "#include <pulp/audio/buffer.hpp>\n#include <pulp/view/meter.hpp>\n")
        self._write("core/audio/include/pulp/audio/buffer.hpp", "#pragma once\n")

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def _run(self, root: pathlib.Path | None = None) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return boundary.main(["--boundary", "view-core", "--root", str(root or self.root)])

    def _plant(self, text: str, name: str = "core/view/src/planted.cpp") -> int:
        self._write(name, text)
        return self._run()

    def test_clean_tree_passes(self):
        # Also proves core/view/audio is outside the scan: meter.cpp includes
        # pulp/audio and the tree is still clean.
        self.assertEqual(self._run(), 0)

    def test_angle_include_fails(self):
        self.assertEqual(self._plant("#include <pulp/audio/buffer.hpp>\n"), 1)

    def test_quoted_include_fails(self):
        self.assertEqual(self._plant('#include "pulp/audio/buffer.hpp"\n'), 1)

    def test_import_fails(self):
        self.assertEqual(self._plant("#import <pulp/signal/fft.hpp>\n",
                                     "core/view/platform/mac/planted.mm"), 1)

    def test_spaced_directive_fails(self):
        self.assertEqual(self._plant('  #  include   "pulp/host/plugin_slot.hpp"\n'), 1)

    def test_every_forbidden_area_fails(self):
        for area in ("audio", "signal", "host", "midi", "format"):
            with self.subTest(area=area):
                self.assertEqual(self._plant(f"#include <pulp/{area}/x.hpp>\n"), 1)

    def test_relative_escape_out_of_view_fails(self):
        self.assertEqual(
            self._plant('#include "../../audio/include/pulp/audio/buffer.hpp"\n'), 1)

    def test_relative_escape_into_the_audio_subtree_fails(self):
        self.assertEqual(
            self._plant('#include "../audio/include/pulp/view/meter.hpp"\n'), 1)

    def test_unresolvable_relative_include_fails(self):
        self.assertEqual(self._plant('#include "../nowhere/x.hpp"\n'), 1)

    def test_audio_widget_header_by_view_path_fails(self):
        # pulp/view/meter.hpp is a view-area name, but only the audio widgets'
        # include directory provides it.
        self.assertEqual(self._plant("#include <pulp/view/meter.hpp>\n"), 1)

    def test_view_header_present_in_core_passes(self):
        self.assertEqual(self._plant('#include "pulp/view/view.hpp"\n'), 0)

    def test_commented_include_is_not_an_edge(self):
        self.assertEqual(self._plant("// #include <pulp/audio/buffer.hpp>\n"), 0)

    def test_allowlisted_edge_passes_and_stale_entry_fails(self):
        key = ("core/view/src/planted.cpp", "pulp/audio/buffer.hpp")
        saved = dict(boundary.ALLOWED["view-core"])
        boundary.ALLOWED["view-core"][key] = "test"
        try:
            self.assertEqual(self._plant("#include <pulp/audio/buffer.hpp>\n"), 0)
            self.assertEqual(self._plant("#include <pulp/view/view.hpp>\n"), 1)
        finally:
            boundary.ALLOWED["view-core"].clear()
            boundary.ALLOWED["view-core"].update(saved)

    def test_empty_tree_is_a_scan_error_not_a_pass(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(self._run(pathlib.Path(empty)), 2)

    def test_repository_is_clean(self):
        self.assertEqual(self._run(REPO), 0)


if __name__ == "__main__":
    unittest.main()
