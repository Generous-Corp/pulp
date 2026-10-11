#!/usr/bin/env python3
"""Positive and negative controls for foundation_include_lint.py."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("foundation_include_lint.py")
SPEC = importlib.util.spec_from_file_location("foundation_include_lint", MODULE_PATH)
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)

REPO = pathlib.Path(__file__).resolve().parents[2]


class FoundationIncludeLintTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self._write("core/platform/src/platform.cpp",
                    '#include <pulp/platform/platform.hpp>\n#include "../include/x.hpp"\n')
        self._write("core/platform/include/x.hpp", "#pragma once\n")
        self._write("core/runtime/src/log.cpp", "#include <pulp/runtime/log.hpp>\n")

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def _run(self, root: pathlib.Path | None = None) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return lint.main(["--root", str(root or self.root)])

    def test_foundation_only_tree_passes(self):
        self.assertEqual(self._run(), 0)

    def test_audio_include_fails(self):
        self._write("core/platform/src/android/jni_bridge.cpp",
                    "#include <pulp/audio/audio_focus.hpp>\n")
        self.assertEqual(self._run(), 1)

    def test_quoted_upper_layer_include_fails(self):
        self._write("core/runtime/src/a.cpp", '#include "pulp/format/processor.hpp"\n')
        self.assertEqual(self._run(), 1)

    def test_relative_include_escaping_foundation_fails(self):
        self._write("core/platform/src/android/jni_bridge.cpp",
                    '#include "../../../audio/platform/android/demo_synth.hpp"\n')
        self.assertEqual(self._run(), 1)

    def test_relative_include_reaching_audio_through_include_dir_fails(self):
        # The historical jni_bridge.cpp spelling: it does not resolve from the
        # file's directory, only from core/platform/include, into core/audio.
        self._write("core/audio/platform/android/demo_synth.hpp", "#pragma once\n")
        self._write("core/platform/include/pulp/platform/platform.hpp", "#pragma once\n")
        self._write("core/platform/src/android/jni_bridge.cpp",
                    '#include "../../audio/platform/android/demo_synth.hpp"\n')
        self.assertEqual(self._run(), 1)

    def test_relative_include_between_foundation_roots_passes(self):
        self._write("core/platform/src/b.cpp", '#include "../../runtime/src/log.cpp"\n')
        self.assertEqual(self._run(), 0)

    def test_view_include_fails(self):
        self._write("core/platform/src/android/jni_motion.cpp",
                    "#include <pulp/view/motion.hpp>\n")
        self.assertEqual(self._run(), 1)

    def test_allowlisted_edge_passes_and_stale_entry_fails(self):
        key = ("core/platform/src/a.cpp", "pulp/view/motion.hpp")
        saved = dict(lint.ALLOWED)
        lint.ALLOWED[key] = "test"
        try:
            self._write(key[0], "#include <pulp/view/motion.hpp>\n")
            self.assertEqual(self._run(), 0)
            self._write(key[0], "#include <pulp/platform/platform.hpp>\n")
            self.assertEqual(self._run(), 1)
        finally:
            lint.ALLOWED.clear()
            lint.ALLOWED.update(saved)

    def test_empty_tree_is_a_scan_error_not_a_pass(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(self._run(pathlib.Path(empty)), 2)

    def test_repository_is_clean(self):
        self.assertEqual(self._run(REPO), 0)


if __name__ == "__main__":
    unittest.main()
